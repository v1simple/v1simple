"""Explicit blind frequency adjudication; original field references stay intact."""
from __future__ import annotations

from collections import Counter
from copy import deepcopy
import ctypes
import errno
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import sys


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
SUPPLEMENTAL_CONTROL_STATES = {
    "extra_ink_control": {"ambiguous", "unreadable"},
    "occluded_camera_control": {"ambiguous", "unreadable"},
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


def _clone_file(source, destination):
    """Return False only when native copy-on-write is unavailable."""
    if sys.platform != "darwin":
        return False
    try:
        clone = ctypes.CDLL(None, use_errno=True).clonefile
    except AttributeError:
        return False
    clone.argtypes = (ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int)
    clone.restype = ctypes.c_int
    if clone(os.fsencode(source), os.fsencode(destination), 0) == 0:
        return True
    error = ctypes.get_errno()
    if error in (errno.ENOTSUP, errno.ENOSYS, errno.EXDEV):
        return False
    raise OSError(error, os.strerror(error), str(destination))


def copy_evidence(source, destination, expected_sha256):
    """Retain independent file semantics and exact bytes without duplicate blocks."""
    source, destination = Path(source), Path(destination)
    _require(not source.is_symlink() and source.is_file()
             and _sha(source) == expected_sha256, "missing, indirect or changed copy source")
    _require(not destination.is_symlink(), "copy destination is a symbolic link")
    if destination.exists():
        _require(destination.is_file() and _sha(destination) == expected_sha256,
                 "copied artifact path collision")
        return
    destination.parent.mkdir(parents=True, exist_ok=True)
    if not _clone_file(source, destination):
        shutil.copy2(source, destination)
    _require(not destination.is_symlink() and destination.is_file()
             and _sha(destination) == expected_sha256, "artifact changed while copying")


def copy_reference(source, destination):
    """Copy only hash-bound packet inputs, including original/control PNGs."""
    _require(not Path(source).is_symlink(), "reference is a symbolic link")
    source, destination = Path(source).resolve(), Path(destination)
    document = json.loads(source.read_bytes())
    refs = [document[name] for name in ("protocol", "blind_manifest", "observations")]
    if "selection_before_reading" in document:
        refs.append(document["selection_before_reading"])
    refs += document.get("provenance_artifacts", [])
    refs += [item["image"] for item in document["items"]]
    for item in document["items"]:
        startup = item.get("startup_calibration")
        if startup is not None:
            _require(isinstance(startup, dict) and set(startup) == {"preflight", "still"},
                     "startup calibration inputs are incomplete")
            refs += [startup["preflight"], startup["still"]]
    destination.parent.mkdir(parents=True, exist_ok=True)
    for ref in refs:
        original = _artifact(source.parent, ref)
        target = destination.parent / ref["path"]
        copy_evidence(original, target, ref["sha256"])
    copy_evidence(source, destination, _sha(source))


def reference_reread_binding(path, method):
    """Describe a current-reader regression check without changing the old freeze.

    The independent packet remains the original observer's work. Its held-out
    development separation belongs to that historical reader, not the new one.
    """
    from encounter_qualification import CORE_READER_FILES, LEGACY_CORE_READER_FILES, READER24_CORE_READER_FILES

    path = Path(path).resolve()
    reference = json.loads(path.read_bytes())
    frozen = reference.get("frozen_reader_files")
    current = {name: method.get(name) for name in CORE_READER_FILES}
    _require(isinstance(frozen, dict)
             and set(frozen) in (set(LEGACY_CORE_READER_FILES), set(READER24_CORE_READER_FILES),
                                 set(CORE_READER_FILES))
             and all(isinstance(value, str) and re.fullmatch(r"[0-9a-f]{64}", value)
                     for value in (*frozen.values(), *current.values())),
             "historical or current reader freeze is incomplete")
    if frozen == current:
        return None
    return {"kind": "complete_exact_reader_reread", "reference_sha256": _sha(path),
            "historical_frozen_reader_files": frozen, "current_reader_files": current,
            "complete_source_set_reread": True,
            "held_out_provenance": "Original frozen reader only; current reader is checked against retained independent labels."}


def _item_registration(item, registration, root, calibrations):
    """Recompute frequency geometry from retained startup pixels, never a matrix."""
    _require(isinstance(registration, dict) and registration.get("result") == "PASS",
             "original image registration is unavailable")
    _require("primary_frequency_calibration" not in registration,
             "stored frequency calibration cannot replace retained startup inputs")
    startup = item.get("startup_calibration")
    if startup is None:
        return registration
    _require(isinstance(startup, dict) and set(startup) == {"preflight", "still"},
             "startup calibration inputs are incomplete")
    preflight_path = _artifact(root, startup["preflight"])
    still_path = _artifact(root, startup["still"])
    preflight = json.loads(preflight_path.read_bytes())
    _require(isinstance(preflight, dict) and preflight.get("result") == "PASS"
             and preflight.get("registration") == registration,
             "startup preflight differs from original image registration")
    source = preflight.get("source_still")
    _require(isinstance(source, dict) and source.get("name") == still_path.name
             and source.get("sha256") == startup["still"]["sha256"],
             "startup preflight identifies a different original still")
    key = (startup["preflight"]["sha256"], startup["still"]["sha256"])
    if key not in calibrations:
        from encounter_frequency_idle import registration_for_camera
        calibrated = registration_for_camera(preflight, still_path)
        _require(isinstance(calibrated, dict)
                 and {k: v for k, v in calibrated.items() if k != "primary_frequency_calibration"} == registration
                 and isinstance(calibrated.get("primary_frequency_calibration"), dict),
                 "startup frequency calibration changed other image registration")
        calibrations[key] = calibrated
    return deepcopy(calibrations[key])


def validate_reference(path, original_manifest, original_observations, method, registration, observe,
                       *, reader_reanalysis=None):
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
    if reader_reanalysis is None:
        _require(reference.get("frozen_reader_files") == {name: method[name] for name in CORE_READER_FILES},
                 "reader changed after the independent observation packet was frozen")
    else:
        expected_binding = reference_reread_binding(path, method)
        _require(expected_binding is not None and reader_reanalysis == expected_binding,
                 "reader reread binding differs from immutable reference or current reader")
    _artifact(path.parent, reference["protocol"])
    if "selection_before_reading" in reference:
        _artifact(path.parent, reference["selection_before_reading"])
    for artifact in reference.get("provenance_artifacts", []):
        _artifact(path.parent, artifact)
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
    overrides, roles, counts, hashes, calibrations = {}, Counter(), Counter(), set(), {}
    coverage, qualified_startups = Counter(), set()
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
        default_registration = reference.get("registration", registration) if role == "held_out_original" else registration
        image_registration = item.get("registration", default_registration)
        image_registration = _item_registration(item, image_registration, path.parent, calibrations)
        observed = observe(image, image_registration)["primary_frequency"]
        status = _derived_field_status("primary_frequency", observed, label)
        _require(status not in ("WRONG_ASSERTION", "ASSERTION_WITHOUT_RESOLVED_REFERENCE"),
                 f"reader contradicts the independent label: {frame_id}")
        if item.get("startup_calibration") is not None:
            coverage["startup_items"] += 1
            calibration = image_registration["primary_frequency_calibration"]
            if calibration.get("qualified") is True:
                qualified_startups.add(item["startup_calibration"]["still"]["sha256"])
                if (observed.get("calibrated_numeric_decimal", {}).get("accepted") is True
                        and status == "AGREEMENT" and observed.get("state") == "readable"
                        and re.fullmatch(r"[0-9]{2}\.[0-9]{3}", observed.get("value", ""))):
                    coverage["calibrated_numeric_decimal_acceptances"] += 1
                diagnostic = observed.get("calibrated_idle", {})
                if diagnostic.get("calibration_qualified") is True:
                    if (diagnostic.get("accepted") is True and status == "AGREEMENT"
                            and observed.get("state") == "readable" and observed.get("value") == "--.---"):
                        coverage["calibrated_idle_acceptances"] += 1
                    elif observed.get("state") in {"ambiguous", "unreadable"}:
                        if diagnostic.get("residual_ink", {}).get("clear") is False:
                            coverage["residual_ink_refusals"] += 1
                        elif diagnostic.get("template_match") is False:
                            coverage["template_refusals"] += 1
                        elif diagnostic.get("complete_dash_decimal_witness") is False:
                            coverage["incomplete_witness_refusals"] += 1
            else:
                coverage["calibration_refusals"] += 1
        if role == "reference_correction":
            source_id = item.get("source_frame_id")
            _require(source_id in originals and source_id not in overrides
                     and originals[source_id]["image_sha256"] == digest,
                     "correction is not an exact original field image")
            overrides[source_id] = label
        elif role in ("held_out_original", "retained_original"):
            origin = item.get("origin", {})
            _require(digest not in {f["image_sha256"] for f in originals.values()}
                     and isinstance(origin.get("capture_id"), str) and origin["capture_id"]
                     and type(origin.get("video_frame_index")) is int and origin["video_frame_index"] >= 0
                     and isinstance(origin.get("video_sha256"), str) and len(origin["video_sha256"]) == 64
                     and (item.get("not_used_for_reader_development") is True if role == "held_out_original"
                          else isinstance(item.get("development_provenance"), str) and item["development_provenance"]),
                     "original identity or development provenance is missing")
            held_out_dashes += int(role == "held_out_original" and status == "AGREEMENT" and label.get("value") == "--.---")
        elif role == "wrong_literal_control":
            _require(isinstance(item.get("operation"), (str, dict)) and item["operation"]
                     and label.get("state") == "readable" and re.fullmatch(r"[0-9]{2}\.[0-9]{3}", label["value"])
                     and status == "AGREEMENT", "complete contrary literal was not read exactly")
        else:
            allowed_states = CONTROL_STATES.get(role, SUPPLEMENTAL_CONTROL_STATES.get(role))
            _require(allowed_states is not None and isinstance(item.get("operation"), (str, dict)) and item["operation"],
                     "control role or declared image operation is missing")
            reference_supported = (label.get("state") in allowed_states
                                   or (label.get("state") == "unresolved" and "ambiguous" in allowed_states)
                                   or (label.get("state") == "readable"
                                       and (label.get("value") in CONTROL_WRONG_LITERALS.get(role, set())
                                            or role == "extra_ink_control" and re.fullmatch(r"[0-9]{2}\.[0-9]{3}", label["value"]))))
            _require(observed.get("state") in allowed_states
                     and reference_supported,
                     f"control did not safely distinguish missing/invalid pixels: {frame_id}")
        roles[role] += 1
        counts[status] += 1
    _require(held_out_dashes >= 2, "fewer than two held-out originals independently resolve the dash literal")
    _require(all(roles[role] for role in CONTROL_STATES), "missing dash/decimal fault-control coverage")
    return {"overrides": overrides, "summary": {"items": len(items), "roles": dict(roles),
            "counts": dict(counts), "held_out_dash_agreements": held_out_dashes,
            "adjudicated_original_references": len(overrides),
            "calibrated_idle_coverage": {
                "qualified_startup_image_sha256": sorted(qualified_startups),
                **{key: coverage[key] for key in ("startup_items", "calibrated_idle_acceptances",
                    "calibrated_numeric_decimal_acceptances",
                    "residual_ink_refusals", "template_refusals", "incomplete_witness_refusals",
                    "calibration_refusals")}},
            **({"reader_reanalysis": reader_reanalysis} if reader_reanalysis is not None else {})}}
