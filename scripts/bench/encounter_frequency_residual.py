"""Refuse an idle-frequency candidate when additional stroke contrast remains.

The frozen background envelope uses 40 original idle display images from one
development recording. It records the largest positive local-background
residual at each fixed observation support. The margin is three additional RGB
levels above that measured envelope, not an absolute three-level sensitivity
claim. A separate geometry, complete-glyph and template match is still required.
"""

import base64
from functools import lru_cache
import hashlib
import io
from pathlib import Path


BOX = (435, 240, 850, 375)
EMPTY_BOXES = ((454, 258, 832, 292), (454, 321, 832, 341),
               (454, 347, 600, 359), (632, 347, 832, 359),
               (524, 294, 540, 318), (598, 294, 631, 318),
               (692, 294, 712, 318), (772, 294, 788, 318))
THIN_SUPPORT = (634, 303, 637, 309)
BACKGROUND_KERNEL = 31
COHERENT_PATCH = 4
CONTRAST_FLOOR = 3.0
MODEL_SHA256 = "b7b2462cd35808a061bf31bc6fa21ec96757125416ffefd46af4899600539dde"
MODEL_SHAPE = (2, 22594)
# Frozen from the declared development images; no event-dependent retraining.
# Their selection and source identities remain bound by this selection digest.
DEVELOPMENT_SELECTION_SHA256 = "1ba9a8f94e592ffe3c228342364888964d7df5f9bf89309b3a87a5591ef6c1c5"


def _dependencies():
    import cv2
    import numpy as np
    return cv2, np


@lru_cache(maxsize=1)
def _model():
    _, np = _dependencies()
    payload = base64.b64decode(
        b"".join(Path(__file__).with_name("encounter_frequency_residual.b64").read_bytes().split()),
        validate=True)
    if hashlib.sha256(payload).hexdigest() != MODEL_SHA256:
        raise ValueError("primary-frequency residual model identity changed")
    with np.load(io.BytesIO(payload), allow_pickle=False) as archive:
        envelope = archive["envelope"].copy()
    if (envelope.shape != MODEL_SHAPE or envelope.dtype != np.float32
            or not np.isfinite(envelope).all() or np.any(envelope < 0)):
        raise ValueError("invalid primary-frequency residual model")
    envelope.flags.writeable = False
    return envelope


def _patches(rgb):
    cv2, np = _dependencies()
    if rgb.shape != (BOX[3] - BOX[1], BOX[2] - BOX[0], 3) or rgb.dtype != np.uint8:
        raise ValueError("frequency residual guard requires canonical uint8 RGB")
    level = rgb.max(axis=2)
    red = np.maximum(0, rgb[:, :, 0].astype(np.int16)
                     - np.maximum(rgb[:, :, 1], rgb[:, :, 2])).astype(np.uint8)
    results = []
    for values in (level, red):
        residual = values.astype(np.float32) - cv2.medianBlur(values, BACKGROUND_KERNEL)
        channel = []
        for x1, y1, x2, y2 in EMPTY_BOXES:
            region = residual[y1 - BOX[1]:y2 - BOX[1], x1 - BOX[0]:x2 - BOX[0]]
            windows = np.lib.stride_tricks.sliding_window_view(region, (COHERENT_PATCH,) * 2)
            channel.append(np.median(windows, axis=(-2, -1)).ravel())
        # This three-pixel extension cannot fill a broad 4x4 support. Preserve
        # the existing reader's complete fixed thin-middle observation.
        x1, y1, x2, y2 = THIN_SUPPORT
        channel.append(np.array([np.median(
            residual[y1 - BOX[1]:y2 - BOX[1], x1 - BOX[0]:x2 - BOX[0]])]))
        results.append(np.concatenate(channel))
    return np.stack(results)


def observe(canonical_rgb):
    """Return a background-stroke veto, never an independently accepted literal.

    Input is RGB sampled once onto BOX in the frozen reference coordinate grid.
    Missing dependencies, invalid pixels or changed model data raise an error;
    the caller must leave that observation unresolved.
    """
    residual = _patches(canonical_rgb) - _model()
    metrics = {
        "level_maximum_coherent_added_contrast": float(residual[0].max()),
        "red_chroma_maximum_coherent_added_contrast": float(residual[1].max()),
    }
    clear = max(metrics.values()) < CONTRAST_FLOOR
    return {"clear": clear,
            "reason": None if clear else "coherent added frequency background stroke contrast",
            **metrics, "additional_contrast_floor": CONTRAST_FLOOR}
