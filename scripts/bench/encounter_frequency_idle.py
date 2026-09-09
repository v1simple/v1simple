"""Startup-calibrated idle-frequency observations from one original RGB frame.

This augments unresolved readings only. A whole-template match, complete dash
and decimal witnesses, and a separate residual-ink check must all agree. It
never receives expected values, timestamps, or neighboring frames.
"""
from copy import deepcopy
import hashlib
import importlib
from importlib.metadata import version, PackageNotFoundError
import io
import json
from pathlib import Path

import numpy as np
from PIL import Image

import encounter_frequency_geometry as geometry


MATCH_BOX = (448, 250, 842, 367)
MATCH_PARAMETERS = dict(match_method="sqdiff", match_threshold=.98,
                        confirm_method="normed-absdiff", confirm_threshold=.70,
                        erode_passes=1)
PYRAMID_LEVELS = 3


def runtime():
    """Bind the libraries that actually interpret pixels, without capture/OCR."""
    try:
        import cv2
        module = importlib.import_module("_stbt.match")
        return {"opencv_version": cv2.__version__, "stbt_core_version": version("stbt-core"),
                "stbt_pyramid_levels": module.get_config("match", "pyramid_levels", type_=int),
                "stbt_match_source_sha256": hashlib.sha256(Path(module.__file__).read_bytes()).hexdigest()}
    except (ImportError, OSError, PackageNotFoundError):
        return {"opencv_version": None, "stbt_core_version": None,
                "stbt_pyramid_levels": None,
                "stbt_match_source_sha256": None}


def registration_for_camera(preflight, still_path):
    """Recompute geometry from the hash-bound startup still, never a saved matrix."""
    registration = deepcopy(preflight.get("registration", {}))
    if (preflight.get("result") != "PASS" or registration.get("result") != "PASS"
            or "primary_frequency_calibration" in registration):
        raise ValueError("primary-frequency startup registration is invalid")
    still_path = Path(still_path)
    source = preflight.get("source_still", {})
    payload = still_path.read_bytes()
    digest = hashlib.sha256(payload).hexdigest()
    if source.get("name") != still_path.name or source.get("sha256") != digest:
        raise ValueError("primary-frequency startup still identity differs")
    if source.get("size_bytes") is not None and source["size_bytes"] != len(payload):
        raise ValueError("primary-frequency startup still size differs")
    with Image.open(io.BytesIO(payload)) as image:
        image = image.convert("RGB")
        try:
            calibration = geometry.calibrate(image.tobytes(), image.width, image.height, registration)
        except ImportError:
            calibration = {"qualified": False, "reason": "primary-frequency image libraries unavailable"}
    calibration.update(startup_image_sha256=digest,
                       preflight_document_sha256=hashlib.sha256(json.dumps(
                           preflight, sort_keys=True, separators=(",", ":"), allow_nan=False).encode()).hexdigest(),
                       model_sha256=geometry.MODEL_SHA256)
    registration["primary_frequency_calibration"] = calibration
    return registration


def refine(reading, rgb, width, height, registration):
    """Resolve an unknown only when all independently fixed pixel checks agree."""
    calibration = registration.get("primary_frequency_calibration")
    if not calibration or reading.get("state") not in ("ambiguous", "unreadable"):
        return reading
    if not isinstance(calibration, dict):
        return {**reading, "calibrated_idle": {
            "calibration_qualified": False, "accepted": False,
            "reason": "malformed startup frequency calibration"}}
    diagnostics = {"calibration_qualified": calibration.get("qualified") is True,
                   "accepted": False}
    if calibration.get("qualified") is not True:
        diagnostics["reason"] = calibration.get("reason", "startup calibration refused")
        return {**reading, "calibrated_idle": diagnostics}
    try:
        import cv2
        import stbt_core as stbt
        import encounter_frequency_residual as residual
        from encounter_reader import Pixels, _dark_frequency_placeholder
        from _stbt.config import get_config

        if get_config("match", "pyramid_levels", type_=int) != PYRAMID_LEVELS:
            raise ValueError("Stb-tester pyramid configuration differs from the fixed reader")
        if (width, height) != (1280, 720) or calibration.get("model_sha256") != geometry.MODEL_SHA256:
            raise ValueError("primary-frequency calibration does not cover this image/model")
        image = np.frombuffer(rgb, dtype=np.uint8).reshape(height, width, 3)
        matrix = np.asarray(calibration["matrix_reference_to_observed"], dtype=np.float64)
        crop = geometry.canonical(image, MATCH_BOX, matrix)
        template = geometry.frequency_template()
        match = stbt.match(np.ascontiguousarray(template[:, :, ::-1]),
                           frame=np.ascontiguousarray(crop[:, :, ::-1]),
                           match_parameters=stbt.MatchParameters(**MATCH_PARAMETERS),
                           region=stbt.Region(0, 0, right=crop.shape[1], bottom=crop.shape[0]))
        diagnostics["template_match"] = bool(match.match)
        diagnostics["template_similarity"] = float(match.first_pass_result)
        if not match.match:
            return {**reading, "calibrated_idle": diagnostics}
        # Preserve the original fixed, complete-stroke witness after correcting
        # geometry. The residual model separately tests the surrounding field.
        aligned = cv2.warpAffine(image, matrix[:2].astype(np.float32), (width, height),
                                 flags=cv2.INTER_LINEAR | cv2.WARP_INVERSE_MAP)
        pixels = Pixels(aligned.tobytes(), width, height, geometry.model_metadata()["reference_registration"])
        placeholder = _dark_frequency_placeholder(pixels, [])
        witness = placeholder.get("dark_placeholder", {})
        strokes = witness.get("strokes", [])
        complete = len(strokes) == 5 and all(s.get("complete") is True for s in strokes) \
            and witness.get("decimal", {}).get("complete") is True
        diagnostics["complete_dash_decimal_witness"] = complete
        if not complete:
            return {**reading, "calibrated_idle": diagnostics}
        guard = residual.observe(geometry.canonical(image, residual.BOX, matrix))
        diagnostics["residual_ink"] = guard
        if not guard["clear"]:
            return {**reading, "calibrated_idle": diagnostics}
        diagnostics["accepted"] = True
        return {"state": "readable", "value": "--.---", "reason": None,
                "calibrated_idle": diagnostics,
                "prior_reading": {key: reading.get(key) for key in ("state", "value", "reason")}}
    except Exception as error:
        # A library or malformed-context failure cannot invalidate other fields
        # or replace the original unresolved frequency observation.
        diagnostics["reason"] = "calibrated idle reading unavailable: " + str(error)
        return {**reading, "calibrated_idle": diagnostics}
