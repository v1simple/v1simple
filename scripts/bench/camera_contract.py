"""Single machine-readable contract for bench camera evidence.

Camera capture has three deliberately different purposes.  Core and display
videos are retained as diagnostic/exercise evidence; only replay has an
independent same-window log and may affect the bench verdict.
"""

from __future__ import annotations

from copy import deepcopy
import math
from typing import Any, Mapping


CAMERA_CONTRACT_SCHEMA_VERSION = 2
REPLAY_MUTE_SIGNAL_SCHEMA = 1
REPLAY_MUTE_EVENT_STATE = "detector_mute"

EXPECTED_CAMERA_NAME = "Global Shutter Camera"
EXPECTED_CAMERA_PROFILE = {
    "auto_exposure_mode": 8,
    "auto_exposure_priority": 0,
    "focus_abs": 306,
    "video_exposure_time_abs": 50,
    "gain": 0,
    "framerate": 200,
    "input_pixel_format": "nv12",
    "video_size": "1280x720",
    "capture_backend": "avfoundation_native",
}
SUPPORTED_GRADER_VIDEO_SIZES = ("1280x720", "1920x1080")

REGISTRATION_SOURCE_FIELDS = ("session_start_still", "bright_still")
MIN_DISPLAY_SCALE = 0.55
MAX_DISPLAY_SCALE = 1.60
MAX_REPLAY_ALIGNMENT_ADJUSTMENT_S = 3.0

MIN_TIMELINE_MATCH_RATIO = 0.96
MIN_ALERT_MATCH_RATIO = 0.95
MIN_FREQUENCY_MATCH_RATIO = 0.90
MIN_DIRECTION_MATCH_RATIO = 0.85
MIN_ALERT_COMPARISONS = 20
MIN_FREQUENCY_COMPARISONS = 20
MIN_DIRECTION_COMPARISONS = 12


class ReplayMuteSignalError(ValueError):
    """The replay mute schedule is missing or cannot be interpreted exactly."""


def normalize_replay_mute_signal(signal: Mapping[str, Any] | None) -> dict[str, Any]:
    """Return the exact canonical mute schedule used by capture and grading."""
    if not isinstance(signal, Mapping):
        raise ReplayMuteSignalError("replay mute signal is missing")
    if (
        type(signal.get("schema_version")) is not int
        or signal["schema_version"] != REPLAY_MUTE_SIGNAL_SCHEMA
    ):
        raise ReplayMuteSignalError("replay mute signal has an unsupported schema")
    events = signal.get("events")
    if not isinstance(events, list):
        raise ReplayMuteSignalError("replay mute signal events must be a list")

    normalized: list[dict[str, Any]] = []
    previous_second = -1.0
    for index, event in enumerate(events):
        if not isinstance(event, Mapping) or event.get("state") != REPLAY_MUTE_EVENT_STATE:
            raise ReplayMuteSignalError(f"replay mute event {index} has an invalid state")
        raw_second = event.get("replaySecond")
        if isinstance(raw_second, bool) or not isinstance(raw_second, (int, float)):
            raise ReplayMuteSignalError(f"replay mute event {index} has an invalid time")
        replay_second = float(raw_second)
        if (
            not math.isfinite(replay_second)
            or replay_second < 0
            or replay_second < previous_second
        ):
            raise ReplayMuteSignalError(f"replay mute event {index} is not ordered")
        muted = event.get("muted")
        if type(muted) is not bool:
            raise ReplayMuteSignalError(f"replay mute event {index} has an invalid muted value")
        normalized.append(
            {
                "state": REPLAY_MUTE_EVENT_STATE,
                "replaySecond": (
                    int(replay_second) if replay_second.is_integer() else replay_second
                ),
                "muted": muted,
            }
        )
        previous_second = replay_second
    return {"schema_version": REPLAY_MUTE_SIGNAL_SCHEMA, "events": normalized}


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
                "transform": "dynamic_similarity",
                "scale_bounds": [MIN_DISPLAY_SCALE, MAX_DISPLAY_SCALE],
                "containment": "full_camera_frame",
            },
            "timeline_alignment": {
                "anchor": "first_emitted_replay_sample",
                "hint_required": True,
                "maximum_adjustment_seconds": MAX_REPLAY_ALIGNMENT_ADJUSTMENT_S,
            },
        },
        "oracle_policy": {
            "display_recognition": "seven_segment_topology",
            "reference_images": "none",
            "ambiguous_reading_policy": "abstain",
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
