"""Reuse unchanged, qualified pixel readings from the exact same recording.

Expected states, settings, event boundaries and judgments are never returned.
The caller recomputes them. This loader performs no decoding or native OCR.
"""
from __future__ import annotations

import gzip
import hashlib
import json
import os
from pathlib import Path

try:
    from .encounter_qualification import CORE_READER_FILES, FIELDS, OCR_RUNTIME_FILES
except ImportError:
    from encounter_qualification import CORE_READER_FILES, FIELDS, OCR_RUNTIME_FILES

# Actual pixel-reader dependencies; current full qualification remains the caller's gate.
PIXEL_READER_FILES = (*CORE_READER_FILES, *OCR_RUNTIME_FILES, "camera_contract.py")


def _require(condition, message):
    if not condition:
        raise ValueError("reading reuse: " + message)


def _sha(path):
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _nonfinite(value):
    raise ValueError("reading reuse: nonfinite JSON value " + value)


def _owned(root, name, digest=None):
    path = root / name
    _require(path.is_file() and path.resolve() == path, "missing or indirect artifact " + name)
    if digest is not None:
        _require(isinstance(digest, str) and _sha(path) == digest, "hash mismatch: " + name)
    return path


def load_reusable_readings(prior_result_path, current_data, current_method, selected_samples, out,
                          *, reader_runtime=None):
    """Validate the complete prior reading stream, then link selected witnesses."""
    try:
        return _load(prior_result_path, current_data, current_method, selected_samples, out, reader_runtime)
    except (KeyError, TypeError, AttributeError, EOFError, UnicodeError) as exc:
        raise ValueError("reading reuse: malformed or incomplete retained evidence") from exc


def _load(prior_result_path, current_data, current_method, selected_samples, out, reader_runtime):
    prior_path, out = Path(prior_result_path).resolve(strict=True), Path(out).resolve(strict=True)
    root = prior_path.parent
    result_bytes = prior_path.read_bytes()
    prior = json.loads(result_bytes, parse_constant=_nonfinite)
    _require(prior.get("schema_version") == 1 and prior.get("kind") == "firmware_visual_behavior"
             and prior.get("result") in ("NO_DIFFERENCES_OBSERVED", "DIFFERENCES_FOUND", "MEASUREMENT_INCOMPLETE")
             and prior.get("errors") == [] and prior.get("reader_qualification", {}).get("status") == "QUALIFIED",
             "prior analysis was not complete with a qualified reader")
    evidence, identity = prior["evidence"], current_data["identity"]
    _require(isinstance(reader_runtime, dict) and bool(reader_runtime)
             and evidence.get("reader") == reader_runtime,
             "pixel-reader runtime differs or is unavailable")
    calibration = current_data.get("registration", {}).get("primary_frequency_calibration")
    _require(evidence.get("primary_frequency_calibration") == calibration,
             "startup frequency calibration differs")
    identity_keys = ("capture_id", "capture_manifest_sha256", "runtime_identity", "camera_artifacts",
                     "window_result_sha256", "stimulus_sha256", "delivery_sha256", "scenario_sha256")
    _require(all(identity.get(k) is not None and evidence.get(k) == identity[k] for k in identity_keys),
             "source recording or firmware identity differs")
    method = {}
    for name in PIXEL_READER_FILES:
        digest = current_method.get(name)
        _require(digest and prior.get("reader_method", {}).get(name) == digest
                 and prior.get("implementation_sha256", {}).get(name) == digest,
                 "pixel-reading method differs: " + name)
        _owned(root, "method/" + name, digest)
        method[name] = digest
    selection_path = _owned(root, "selection.json", evidence.get("selection_sha256"))
    _require(isinstance(evidence.get("selection_sha256"), str), "selection hash is missing")
    selection = json.loads(selection_path.read_bytes(), parse_constant=_nonfinite)
    _require(selection.get("kind") == "authored_event_full_frame_selection"
             and all(selection.get("identity", {}).get(k) == identity[k] for k in identity_keys),
             "prior selection belongs to a different recording")
    rows, expected, selected = current_data["rows"], {}, {}
    for label, samples, target in (("prior", selection["samples"], expected), ("current", selected_samples, selected)):
        for sample in samples:
            index, capture = sample.get("video_frame_index"), sample.get("capture_ns")
            _require(type(index) is int and 0 <= index < len(rows) and index not in target
                     and type(capture) is int and capture == rows[index]["host_capture_ns"]
                     and sample.get("source_frame_seq") == rows[index]["frame_seq"],
                     label + " selection contradicts the source sidecar")
            target[index] = sample
    _require(bool(selected) and selected.keys() <= expected.keys(), "requested frames were not previously read")
    raw_path = _owned(root, "readings.ndjson.gz", evidence.get("readings_sha256"))
    _require(isinstance(evidence.get("readings_sha256"), str), "raw reading hash is missing")
    readings, seen = {}, set()
    with gzip.open(raw_path, "rt", encoding="utf-8") as stream:
        for line in stream:
            record = json.loads(line, parse_constant=_nonfinite)
            index = record.get("video_frame_index")
            _require(type(index) is int and index in expected and index not in seen
                     and record.get("capture_ns") == expected[index]["capture_ns"]
                     and record.get("frame_id") == expected[index]["frame_id"],
                     "raw reading is duplicated or disagrees with its selected original")
            reading = record.get("observed")
            _require(isinstance(reading, dict) and isinstance(reading.get("fields"), dict)
                     and set(reading["fields"]) == set(FIELDS)
                     and all(isinstance(value, dict) for value in reading["fields"].values()),
                     "raw reading lacks the complete literal field record")
            seen.add(index)
            if index in selected:
                readings[index] = reading
    _require(seen == expected.keys() and _sha(raw_path) == evidence["readings_sha256"],
             "prior raw analysis is incomplete or changed while reading")
    originals, pending, witness_seen = {}, [], set()
    for item in prior["samples_index"]:
        index = item.get("video_frame_index")
        _require(type(index) is int and index in expected and index not in witness_seen
                 and item.get("frame_index") == index
                 and item.get("frame_id") == expected[index]["frame_id"]
                 and item.get("source_frame_seq") == rows[index]["frame_seq"]
                 and item.get("capture_ns") == expected[index]["capture_ns"]
                 and item.get("image") == f"frames/{index:06d}.png"
                 and isinstance(item.get("image_sha256"), str), "original witness index is invalid")
        witness_seen.add(index)
        source = _owned(root, item["image"], item["image_sha256"])
        if index in selected:
            destination = out / item["image"]
            _require(not destination.exists() and not destination.is_symlink(), "output witness already exists")
            originals[index] = {"image": item["image"], "image_sha256": item["image_sha256"]}
            pending.append((source, destination))
    frames = out / "frames"
    _require(not frames.is_symlink() and (not frames.exists() or frames.is_dir()), "output frames path is indirect")
    created = []
    try:
        frames.mkdir(exist_ok=True)
        for source, destination in pending:
            os.link(source, destination)
            created.append(destination)
    except OSError:
        for path in created:
            path.unlink()
        raise
    return {"readings": readings, "originals": originals, "provenance": {
        "kind": "same_recording_qualified_pixel_readings", "schema_version": 1,
        "prior_result": os.path.relpath(prior_path, out),
        "prior_result_sha256": hashlib.sha256(result_bytes).hexdigest(),
        "readings_sha256": evidence["readings_sha256"], "selection_sha256": evidence["selection_sha256"],
        "reader_method": method, "reader_runtime": reader_runtime,
        "primary_frequency_calibration": calibration,
        "source_identity": {k: identity[k] for k in identity_keys},
        "dependency_inventory": {"pixel_reader": list(CORE_READER_FILES),
                                 "ocr_runtime_probe": list(OCR_RUNTIME_FILES),
                                 "counter_geometry": ["camera_contract.py"]},
        "readings_reused": len(readings), "original_witnesses_linked": len(originals),
        "meaning": "Original pixel readings only; settings, expectations and behavior judgments are recomputed."}}
