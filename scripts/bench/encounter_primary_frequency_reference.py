"""Explicit blind frequency adjudication; original field references stay intact."""
from __future__ import annotations

from collections import Counter
import hashlib
import json
from pathlib import Path
import shutil


BLIND_PROTOCOL = {
    "observations_completed_before_key_access": True,
    "observer_received_machine_output": False,
    "observer_received_hidden_key": False,
    "observer_received_source_or_expectations": False,
}
CONTROL_STATES = {
    "missing_region_control": {"absent"},
    "missing_glyph_control": {"ambiguous", "unreadable"},
    "partial_glyph_control": {"ambiguous", "unreadable"},
    "invalid_glyph_control": {"ambiguous", "unreadable"},
    "missing_decimal_control": {"ambiguous", "unreadable"},
    "partial_decimal_control": {"ambiguous", "unreadable"},
}
# These are literal missing-character controls, not valid placeholder readings.
CONTROL_WRONG_LITERALS = {
    "missing_glyph_control": {"-.---", "--.--"},
    "missing_decimal_control": {"-----"},
}


def _require(condition, message):
    if not condition:
        raise ValueError("primary frequency reference: " + message)


def _sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _artifact(root, ref):
    _require(isinstance(ref, dict) and isinstance(ref.get("path"), str), "malformed artifact reference")
    relative = Path(ref["path"])
    _require(not relative.is_absolute() and ".." not in relative.parts, "artifact path escapes packet")
    path = root / relative
    _require(path.is_file() and path.resolve() == path and _sha(path) == ref.get("sha256"),
             "missing, indirect or changed artifact: " + ref["path"])
    return path


def copy_reference(source, destination):
    """Copy only hash-bound packet inputs, including original/control PNGs."""
    source, destination = Path(source).resolve(), Path(destination)
    document = json.loads(source.read_bytes())
    refs = [document[name] for name in ("protocol", "blind_manifest", "observations")]
    if "selection_before_reading" in document:
        refs.append(document["selection_before_reading"])
    refs += [item["image"] for item in document["items"]]
    destination.parent.mkdir(parents=True, exist_ok=True)
    for ref in refs:
        original = _artifact(source.parent, ref)
        target = destination.parent / ref["path"]
        target.parent.mkdir(parents=True, exist_ok=True)
        _require(not target.exists() or _sha(target) == ref["sha256"], "copied artifact path collision")
        shutil.copyfile(original, target)
    shutil.copyfile(source, destination)


def validate_reference(path, original_manifest, original_observations, method, registration, observe):
    """Re-read an explicit independent supplement, returning only supported overrides."""
    from encounter_qualification import CORE_READER_FILES, _derived_field_status

    path = Path(path).resolve()
    reference = json.loads(path.read_bytes())
    _require(reference.get("schema_version") == 1
             and reference.get("kind") == "independent_primary_frequency_reference",
             "unsupported supplement")
    _require(reference.get("original_blind_manifest_sha256") == _sha(original_manifest)
             and reference.get("original_blind_observations_sha256") == _sha(original_observations),
             "supplement does not bind the unchanged original references")
    _require(reference.get("frozen_reader_files") == {name: method[name] for name in CORE_READER_FILES},
             "reader changed after the independent observation packet was frozen")
    _artifact(path.parent, reference["protocol"])
    if "selection_before_reading" in reference:
        _artifact(path.parent, reference["selection_before_reading"])
    manifest_path = _artifact(path.parent, reference["blind_manifest"])
    manifest = json.loads(manifest_path.read_bytes())
    labels = json.loads(_artifact(path.parent, reference["observations"]).read_bytes())
    _require(labels.get("schema_version") == 1 and labels.get("kind") == "blind_primary_frequency_observations"
             and labels.get("blind_protocol") == BLIND_PROTOCOL, "independent observation provenance is incomplete")
    originals = {f["frame_id"]: f for f in json.loads(original_manifest.read_bytes())["frames"]}
    items = reference.get("items")
    _require(isinstance(items, list) and items, "supplement contains no items")
    packet = {f["frame_id"]: f for f in manifest["frames"]}
    observations = {f["frame_id"]: f for f in labels["frames"]}
    ids = [item["frame_id"] for item in items]
    _require(len(set(ids)) == len(ids) == len(packet) == len(manifest["frames"])
             == len(observations) == len(labels["frames"])
             and set(ids) == set(packet) == set(observations), "packet/label identities are incomplete or duplicated")
    overrides, roles, counts, hashes = {}, Counter(), Counter(), set()
    held_out_dashes = 0
    for item in items:
        frame_id, role, image_ref = item["frame_id"], item["role"], item["image"]
        image = _artifact(path.parent, image_ref)
        digest = image_ref["sha256"]
        _require(digest not in hashes and _artifact(manifest_path.parent, packet[frame_id].get("image")) == image
                 and observations[frame_id].get("image_sha256") == digest,
                 "image is duplicated or differs from its blind packet/label")
        hashes.add(digest)
        label = observations[frame_id].get("primary_frequency")
        _require(isinstance(label, dict) and label.get("state") in ("readable", "absent", "ambiguous", "unreadable", "unresolved")
                 and isinstance(label.get("reason"), str)
                 and (isinstance(label.get("value"), str) if label["state"] == "readable" else label.get("value") is None),
                 "invalid literal primary-frequency label")
        image_registration = reference.get("registration", registration) if role == "held_out_original" else registration
        _require(isinstance(image_registration, dict) and image_registration.get("result") == "PASS",
                 "original image registration is unavailable")
        observed = observe(image, image_registration)["primary_frequency"]
        status = _derived_field_status("primary_frequency", observed, label)
        _require(status not in ("WRONG_ASSERTION", "ASSERTION_WITHOUT_RESOLVED_REFERENCE"),
                 f"reader contradicts the independent label: {frame_id}")
        if role == "reference_correction":
            source_id = item.get("source_frame_id")
            _require(source_id in originals and source_id not in overrides
                     and originals[source_id]["image_sha256"] == digest,
                     "correction is not an exact original field image")
            overrides[source_id] = label
        elif role == "held_out_original":
            origin = item.get("origin", {})
            _require(digest not in {f["image_sha256"] for f in originals.values()}
                     and isinstance(origin.get("capture_id"), str) and origin["capture_id"]
                     and type(origin.get("video_frame_index")) is int and origin["video_frame_index"] >= 0
                     and isinstance(origin.get("video_sha256"), str) and len(origin["video_sha256"]) == 64
                     and item.get("not_used_for_reader_development") is True,
                     "held-out original identity or development separation is missing")
            held_out_dashes += int(status == "AGREEMENT" and label.get("value") == "--.---")
        else:
            _require(role in CONTROL_STATES and isinstance(item.get("operation"), (str, dict)) and item["operation"],
                     "control role or declared image operation is missing")
            reference_supported = (label.get("state") in CONTROL_STATES[role]
                                   or (label.get("state") == "unresolved" and "ambiguous" in CONTROL_STATES[role])
                                   or (label.get("state") == "readable"
                                       and label.get("value") in CONTROL_WRONG_LITERALS.get(role, set())))
            _require(observed.get("state") in CONTROL_STATES[role]
                     and reference_supported,
                     f"control did not safely distinguish missing/invalid pixels: {frame_id}")
        roles[role] += 1
        counts[status] += 1
    _require(held_out_dashes >= 2, "fewer than two held-out originals independently resolve the dash literal")
    _require(all(roles[role] for role in CONTROL_STATES), "missing dash/decimal fault-control coverage")
    return {"overrides": overrides, "summary": {"items": len(items), "roles": dict(roles),
            "counts": dict(counts), "held_out_dash_agreements": held_out_dashes,
            "adjudicated_original_references": len(overrides)}}
