"""Single machine-readable contract for bench camera evidence.

Camera capture has three deliberately different purposes.  Core and display
videos are retained as diagnostic/exercise evidence; only replay has an
independent same-window log and may affect the bench verdict.
"""

from __future__ import annotations

from copy import deepcopy
from typing import Any


CAMERA_CONTRACT_SCHEMA_VERSION = 1

REGISTRATION_SOURCE_FIELDS = ("session_start_still", "bright_still")
MAX_DISPLAY_CROP_OFFSET_X = 96.0
MAX_DISPLAY_CROP_OFFSET_Y = 36.0
MAX_REPLAY_ALIGNMENT_ADJUSTMENT_S = 3.0

MIN_TIMELINE_MATCH_RATIO = 0.96
MIN_ALERT_MATCH_RATIO = 0.95
MIN_FREQUENCY_MATCH_RATIO = 0.90
MIN_DIRECTION_MATCH_RATIO = 0.85
MIN_ALERT_COMPARISONS = 20
MIN_FREQUENCY_COMPARISONS = 20
MIN_DIRECTION_COMPARISONS = 12


_SUITE_CONTRACTS: dict[str, dict[str, Any]] = {
    "core": {
        "role": "diagnostic_capture",
        "gate_required": False,
        "summary": "diagnostic only",
        "purpose": (
            "Retain the idle/core window for diagnosis. This capture has no independent "
            "display oracle and cannot change the bench verdict."
        ),
    },
    "display": {
        "role": "display_exercise_capture",
        "gate_required": False,
        "summary": "exercise only",
        "purpose": (
            "Retain the deterministic preview exercise for inspection and grader development. "
            "It is not replay evidence and cannot change the bench verdict."
        ),
    },
    "replay": {
        "role": "gated_replay_validator",
        "gate_required": True,
        "summary": "gated replay validator",
        "purpose": (
            "Mechanically compare the displayed replay with the same-window encounter log. "
            "A valid mismatch changes the bench verdict; an ungradable capture is an evidence "
            "failure, not a product failure."
        ),
        "calibration_policy": {
            "registration_sources": list(REGISTRATION_SOURCE_FIELDS),
            "display_crop": {
                "transform": "translation_only",
                "maximum_offset_pixels": [
                    MAX_DISPLAY_CROP_OFFSET_X,
                    MAX_DISPLAY_CROP_OFFSET_Y,
                ],
            },
            "timeline_alignment": {
                "anchor": "first_emitted_replay_sample",
                "hint_required": True,
                "maximum_adjustment_seconds": MAX_REPLAY_ALIGNMENT_ADJUSTMENT_S,
            },
        },
        "oracle_policy": {
            "camera_reference": "human_verified_committed_images",
            "encounter_log": "same_window",
            "artifact_ownership_required": True,
            "minimum_match_ratios": {
                "timeline": MIN_TIMELINE_MATCH_RATIO,
                "alerts": MIN_ALERT_MATCH_RATIO,
                "frequencies": MIN_FREQUENCY_MATCH_RATIO,
                "directions": MIN_DIRECTION_MATCH_RATIO,
            },
            "minimum_comparisons": {
                "alerts": MIN_ALERT_COMPARISONS,
                "frequencies": MIN_FREQUENCY_COMPARISONS,
                "directions": MIN_DIRECTION_COMPARISONS,
            },
        },
    },
}


def camera_evidence_contract(suite: str) -> dict[str, Any]:
    """Return an isolated contract payload suitable for an artifact."""
    try:
        contract = deepcopy(_SUITE_CONTRACTS[suite])
    except KeyError as exc:
        raise ValueError(f"unsupported camera suite: {suite}") from exc
    return {
        "schema_version": CAMERA_CONTRACT_SCHEMA_VERSION,
        "suite": suite,
        **contract,
    }


def camera_grade_required(suite: str, capture_result: str) -> bool:
    contract = camera_evidence_contract(suite)
    return bool(contract["gate_required"] and capture_result == "CAPTURED")
