"""Startup-only geometry for the primary-frequency reader.

The SCAN landmark fits an affine transform; a separate startup counter validates
it without changing it. No event image or expected frequency enters calibration.
The bundled model contains only three display crops and their source hashes.
OpenCV and NumPy are loaded on use. Matrices map reference to observed pixels.
"""

import base64
import copy
from functools import lru_cache
import hashlib
import io
import json
from pathlib import Path


MODEL_SHA256 = "7abf3288881bb09e6d39dffcfdf18001877df780c0b458baba805edc98c0aa0f"
CONFIG = {
    "validation_revision": 2,
    "ecc_iterations": 100,
    "ecc_epsilon": 1e-6,
    "gaussian_filter_size": 5,
    "minimum_roi_std": 15.,
    "minimum_refinement_singular_value": .97,
    "maximum_refinement_singular_value": 1.03,
    "maximum_refinement_off_diagonal": .02,
    "maximum_scan_corner_refinement_pixels": 8.,
    "contour_blur_kernel": 5,
    "edge_distance_percentile": 95,
    "maximum_symmetric_edge_distance_pixels": 1.5,
    "maximum_counter_translation_residual_pixels": .75,
}


def _dependencies():
    import cv2
    import numpy as np
    return cv2, np


@lru_cache(maxsize=1)
def _model():
    _, np = _dependencies()
    payload = base64.b64decode(
        b"".join(Path(__file__).with_name("encounter_frequency_model.b64").read_bytes().split()),
        validate=True)
    if hashlib.sha256(payload).hexdigest() != MODEL_SHA256:
        raise ValueError("primary-frequency reference model identity changed")
    with np.load(io.BytesIO(payload), allow_pickle=False) as archive:
        result = {name: archive[name].copy() for name in
                  ("scan_rgb", "counter_rgb", "frequency_rgb")}
        result["metadata"] = json.loads(archive["metadata"].tobytes())
    for name in ("scan_rgb", "counter_rgb", "frequency_rgb"):
        result[name].flags.writeable = False
    return result


def model_metadata():
    """Return reference coordinates and provenance without exposing cached state."""
    return copy.deepcopy(_model()["metadata"])


def frequency_template():
    """Return the frozen RGB frequency template, with no event-dependent fitting."""
    return _model()["frequency_rgb"].copy()


def _orange_level(image):
    _, np = _dependencies()
    value = image.astype(np.float32)
    return np.maximum(0, value[:, :, 0] - np.maximum(value[:, :, 1], value[:, :, 2]))


def _homogeneous(matrix):
    _, np = _dependencies()
    result = np.eye(3, dtype=np.float64)
    result[:2] = matrix
    return result


def _translation(x, y):
    _, np = _dependencies()
    result = np.eye(3, dtype=np.float64)
    result[:2, 2] = [x, y]
    return result


def _subimage(image, box):
    x1, y1, x2, y2 = box
    return image[y1:y2, x1:x2]


def _normalized_correlation(left, right):
    _, np = _dependencies()
    a, b = left.astype(float).ravel(), right.astype(float).ravel()
    a -= a.mean()
    b -= b.mean()
    norm = np.linalg.norm(a) * np.linalg.norm(b)
    return float(a.dot(b) / norm) if norm > 0 else None


def _centroid(image):
    _, np = _dependencies()
    weights = np.maximum(image.astype(float) - np.median(image), 0)
    total = weights.sum()
    y, x = np.indices(image.shape)
    return np.array([(weights*x).sum()/total, (weights*y).sum()/total]) if total > 0 else None


def _contour(image):
    cv2, np = _dependencies()
    image = cv2.GaussianBlur(image, (CONFIG["contour_blur_kernel"],)*2, 0)
    threshold = (np.median(image) + np.percentile(image, 95)) / 2
    mask = (image > threshold).astype(np.uint8)
    return mask - cv2.erode(mask, np.ones((3, 3), np.uint8))


def _edge_agreement(left, right):
    cv2, np = _dependencies()
    a, b = _contour(left), _contour(right)
    if not a.any() or not b.any():
        return {"qualified": False, "reason": "missing contour"}
    da = cv2.distanceTransform(1-a, cv2.DIST_L2, cv2.DIST_MASK_PRECISE)
    db = cv2.distanceTransform(1-b, cv2.DIST_L2, cv2.DIST_MASK_PRECISE)
    forward = float(np.percentile(db[a > 0], CONFIG["edge_distance_percentile"]))
    reverse = float(np.percentile(da[b > 0], CONFIG["edge_distance_percentile"]))
    return {
        "qualified": max(forward, reverse) <= CONFIG["maximum_symmetric_edge_distance_pixels"],
        "reference_to_observed_p95_pixels": forward,
        "observed_to_reference_p95_pixels": reverse,
        "reference_edge_pixels": int(a.sum()), "observed_edge_pixels": int(b.sum()),
    }


def _registration_bounds(registration):
    _, np = _dependencies()
    if not isinstance(registration, dict) or registration.get("result") != "PASS":
        raise ValueError("startup registration is not qualified")
    bounds = registration.get("landmark_bounds")
    if (not isinstance(bounds, (list, tuple)) or len(bounds) != 4
            or any(type(value) not in (int, float) or not np.isfinite(value) for value in bounds)):
        raise ValueError("invalid startup landmark bounds")
    x1, y1, x2, y2 = bounds
    if not (0 <= x1 < x2 < 960 and 0 <= y1 < y2 < 540):
        raise ValueError("startup landmark leaves normalized image")
    return bounds


def calibrate(rgb, width, height, registration):
    """Fit original 1280x720 RGB bytes/array and return JSON-safe diagnostics.

    Refused geometry has qualified=False and must never be applied to events.
    Correlation and centroid values remain diagnostics; the frozen v2 acceptance
    checks use contrast, bounded affine refinement, contours and counter location.
    """
    cv2, np = _dependencies()
    result = {"qualified": False, "validation_revision": 2}
    if (width, height) != (1280, 720):
        return {**result, "reason": "primary-frequency geometry requires 1280x720 originals"}
    try:
        observed_bounds = _registration_bounds(registration)
        if isinstance(rgb, np.ndarray):
            if rgb.dtype != np.uint8 or rgb.shape != (height, width, 3):
                raise ValueError("invalid startup RGB array")
            observed = rgb
        else:
            observed = np.frombuffer(rgb, dtype=np.uint8).reshape(height, width, 3)
    except (ValueError, TypeError):
        return {**result, "reason": "invalid startup RGB or registration"}

    model = _model()
    metadata = model["metadata"]
    scan, zero = metadata["scan_roi"], metadata["counter_validation_roi"]
    r, o = metadata["reference_registration"]["landmark_bounds"], observed_bounds
    sx, sy = (o[2]-o[0]+1)/(r[2]-r[0]+1), (o[3]-o[1]+1)/(r[3]-r[1]+1)
    coarse = np.array([[sx, 0, (o[0]-sx*r[0])*4/3],
                       [0, sy, (o[1]-sy*r[1])*4/3], [0, 0, 1]], dtype=np.float64)
    obs = _orange_level(observed)
    template = _orange_level(model["scan_rgb"])
    prealigned = cv2.warpAffine(obs, coarse[:2].astype(np.float32), (width, height),
                               flags=cv2.INTER_LINEAR | cv2.WARP_INVERSE_MAP)
    moving = _subimage(prealigned, scan)
    result.update({"scan_roi": list(scan), "counter_validation_roi": list(zero),
                   "coarse_matrix": coarse.tolist(), "scan_template_std": float(template.std()),
                   "scan_observed_std": float(moving.std())})
    if min(template.std(), moving.std()) < CONFIG["minimum_roi_std"]:
        return {**result, "reason": "insufficient startup SCAN contrast"}
    criteria = (cv2.TERM_CRITERIA_COUNT | cv2.TERM_CRITERIA_EPS,
                CONFIG["ecc_iterations"], CONFIG["ecc_epsilon"])
    try:
        score, delta = cv2.findTransformECC(
            template, moving, np.eye(2, 3, dtype=np.float32), cv2.MOTION_AFFINE,
            criteria, None, CONFIG["gaussian_filter_size"])
    except cv2.error:
        return {**result, "reason": "startup SCAN alignment did not converge"}
    x1, y1, x2, y2 = scan
    refinement = _translation(x1, y1) @ _homogeneous(delta) @ _translation(-x1, -y1)
    matrix = coarse @ refinement
    singular = np.linalg.svd(delta[:, :2], compute_uv=False)
    corners = np.array([[0, 0, 1], [x2-x1-1, 0, 1], [0, y2-y1-1, 1],
                        [x2-x1-1, y2-y1-1, 1]], dtype=float).T
    movement = float(np.linalg.norm((_homogeneous(delta) @ corners-corners)[:2], axis=0).max())
    aligned = cv2.warpAffine(obs, matrix[:2].astype(np.float32), (width, height),
                            flags=cv2.INTER_LINEAR | cv2.WARP_INVERSE_MAP)
    zero_ref, zero_obs = _orange_level(model["counter_rgb"]), _subimage(aligned, zero)
    correlation = _normalized_correlation(zero_ref, zero_obs)
    a, b = _centroid(zero_ref), _centroid(zero_obs)
    center_error = float(np.linalg.norm(a-b)) if a is not None and b is not None else None
    checks = {
        "refinement_scale": float(singular.min()) >= CONFIG["minimum_refinement_singular_value"]
        and float(singular.max()) <= CONFIG["maximum_refinement_singular_value"],
        "refinement_cross_terms": max(abs(float(delta[0, 1])), abs(float(delta[1, 0])))
        <= CONFIG["maximum_refinement_off_diagonal"],
        "refinement_movement": movement <= CONFIG["maximum_scan_corner_refinement_pixels"],
        "counter_contrast": min(zero_ref.std(), zero_obs.std()) >= CONFIG["minimum_roi_std"],
    }
    # Preserve the old diagnostic checks so extraction parity remains reviewable.
    v1_checks = {**checks, "scan_correlation": score >= .98,
                 "counter_correlation": correlation is not None and correlation >= .98,
                 "counter_centroid": center_error is not None and center_error <= .75}
    scan_edges = _edge_agreement(template, _subimage(aligned, scan))
    zero_edges = _edge_agreement(zero_ref, zero_obs)
    try:
        counter_cc, counter_delta = cv2.findTransformECC(
            zero_ref, zero_obs, np.eye(2, 3, dtype=np.float32), cv2.MOTION_TRANSLATION,
            criteria, None, CONFIG["gaussian_filter_size"])
        counter_cc = float(counter_cc)
        residual = float(np.linalg.norm(counter_delta[:, 2]))
        residual_xy = counter_delta[:, 2].tolist()
    except cv2.error:
        counter_cc = residual = residual_xy = None
    checks.update({"scan_edges": scan_edges["qualified"], "counter_edges": zero_edges["qualified"],
                   "counter_translation": residual is not None
                   and residual <= CONFIG["maximum_counter_translation_residual_pixels"]})
    result.update({
        "scan_ecc": float(score), "refinement_matrix": refinement.tolist(),
        "matrix_reference_to_observed": matrix.tolist(), "refinement_singular_values": singular.tolist(),
        "scan_corner_refinement_pixels": movement, "counter_correlation": correlation,
        "counter_reference_std": float(zero_ref.std()), "counter_observed_std": float(zero_obs.std()),
        "counter_centroid_error_pixels": center_error, "scan_edges": scan_edges, "counter_edges": zero_edges,
        "counter_translation_fit_correlation": counter_cc, "counter_translation_residual_xy": residual_xy,
        "counter_translation_residual_pixels": residual,
        "v1_checks": {key: bool(value) for key, value in v1_checks.items()},
        "checks": {key: bool(value) for key, value in checks.items()}, "qualified": all(checks.values()),
    })
    result["reason"] = None if result["qualified"] else "startup geometry validation refused: " + ",".join(
        key for key, value in checks.items() if not value)
    return result


def canonical(rgb, box, matrix):
    """Sample reference-coordinate centers once directly from original RGB pixels."""
    cv2, np = _dependencies()
    if not isinstance(rgb, np.ndarray) or rgb.dtype != np.uint8 or rgb.shape != (720, 1280, 3):
        raise ValueError("primary-frequency sampling requires a 1280x720 RGB array")
    matrix = np.asarray(matrix, dtype=np.float64)
    if (matrix.shape != (3, 3) or not np.isfinite(matrix).all()
            or not np.array_equal(matrix[2], [0, 0, 1])):
        raise ValueError("invalid primary-frequency affine matrix")
    if len(box) != 4 or any(type(value) is not int for value in box):
        raise ValueError("invalid primary-frequency sample box")
    x1, y1, x2, y2 = box
    if x1 >= x2 or y1 >= y2:
        raise ValueError("empty primary-frequency sample box")
    metadata = _model()["metadata"]
    anchor, scale = metadata["reference_anchor"], metadata["reference_scale"]
    xx, yy = np.meshgrid(np.arange(x1, x2, dtype=np.float32)+.5,
                         np.arange(y1, y2, dtype=np.float32)+.5)
    rx = anchor[0] + (xx-376*4/3)*scale[0]-.5
    ry = anchor[1] + (yy-192*4/3)*scale[1]-.5
    mx = matrix[0, 0]*rx + matrix[0, 1]*ry + matrix[0, 2]
    my = matrix[1, 0]*rx + matrix[1, 1]*ry + matrix[1, 2]
    if mx.min() < 0 or my.min() < 0 or mx.max() >= rgb.shape[1]-1 or my.max() >= rgb.shape[0]-1:
        raise ValueError("primary-frequency measurement leaves original image")
    return cv2.remap(rgb, mx.astype(np.float32), my.astype(np.float32), cv2.INTER_LINEAR)
