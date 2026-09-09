"""Fail-closed qualification gate for the external encounter reader.

The bench result may use a reader only when a retained qualification bundle
binds the exact reader implementation, runtime, camera profile, blind field
validation, product fault controls, and every temporal classifier allowed by
the visible-event policy.  This module never creates qualification evidence.
It only verifies a caller-supplied bundle and reports why it is unusable.
"""

from __future__ import annotations

from collections import Counter
from copy import deepcopy
import hashlib
import json
import math
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
from typing import Any

try:
    from .encounter_expectation import compare_sample
except ImportError:
    from encounter_expectation import compare_sample


SCHEMA_VERSION = 1
LEGACY_CORE_READER_FILES = (
    "encounter_reader.py", "encounter_ocr.swift", "encounter_ocr_session.py", "counter_reader.py")
READER24_CORE_READER_FILES = (
    *LEGACY_CORE_READER_FILES,
    "encounter_frequency_geometry.py", "encounter_frequency_model.b64", "encounter_frequency_idle.py",
    "encounter_frequency_residual.py", "encounter_frequency_residual.b64")
CORE_READER_FILES = (
    *READER24_CORE_READER_FILES, "encounter_card_text.py", "encounter_frequency_numeric.py")
OCR_RUNTIME_FILES = ("encounter_runtime_probe.py", "encounter_ocr_probe.b64")
STATIC_READER_IMPLEMENTATION_FILES = (
    *CORE_READER_FILES,
    *OCR_RUNTIME_FILES,
    "encounter_expectation.py",
    "counter_expectation.py",
    "camera_contract.py",
    "encounter_qualification.py",
    "encounter_primary_frequency_reference.py",
    "encounter_secondary_reference.py",
)
FIELDS = ("counter_glyph", "primary_frequency", "active_bands", "main_arrows",
          "main_bars", "secondary", "muted_badge")
MINIMUM_BLIND_FRAMES = 20
MINIMUM_AGREEMENTS_PER_FIELD = 10
MINIMUM_NONEMPTY_SECONDARY_AGREEMENTS = 5
MINIMUM_PARTIAL_SECONDARY_IDENTITY_AGREEMENTS = 5
MINIMUM_EMPTY_SECONDARY_PRESENCE_CONTROLS = 5
MINIMUM_TEMPORAL_POSITIVES = 5
MINIMUM_TEMPORAL_NEGATIVES = 5
TEMPORAL_CANDIDATE_SELECTION = {
    "rule": "ALL_ADMISSIONS_AND_HASHED_REJECTIONS_V1",
    "maximum_rejections": 12,
}
TEMPORAL_OBSERVER_CONTEXT_NS_EACH_SIDE = 300_000_000
VISIBLE_SECONDARY_SOURCE_NAMES = (
    "packet_manifest", "blind_labels", "sealed_key", "adjudication")
TEMPORAL_SOURCE_NAMES = (
    "completed_observations", "frozen_classifier_result", "observer_manifest",
    "observer_readme", "pre_pixel_freeze", "restricted_hidden_key", "seal", "selection")
TEMPORAL_V2_SOURCE_NAMES = (
    *TEMPORAL_SOURCE_NAMES, "analysis_selection", "qualification_capture", "window_result",
    "capture_manifest", "qualification_video", "frame_timing", "video_timing_verification",
    "analysis_result")
TEMPORAL_V2_INTEGRITY_CHECKS = (
    "source_artifacts_verified", "blind_ids_match", "clips_match_seal",
    "hidden_decisions_match_frozen", "record_hashes_match", "capture_boundary_verified",
    "retained_capture_verified", "lossless_clip_pixels_verified")
TEMPORAL_SOURCE_HASH_FIELDS = {
    "completed_observations": "completed_observations_sha256",
    "frozen_classifier_result": "frozen_classifier_result_sha256",
    "observer_manifest": "observer_manifest_sha256",
    "observer_readme": "observer_readme_sha256",
    "pre_pixel_freeze": "pre_pixel_freeze_sha256",
    "restricted_hidden_key": "restricted_hidden_key_sha256",
    "seal": "seal_sha256",
    "selection": "selection_sha256",
    "analysis_selection": "analysis_selection_sha256",
    "qualification_capture": "qualification_capture_sha256",
    "window_result": "window_result_sha256",
    "capture_manifest": "capture_manifest_sha256",
    "qualification_video": "qualification_video_sha256",
    "frame_timing": "frame_timing_sha256",
    "video_timing_verification": "video_timing_verification_sha256",
    "analysis_result": "analysis_result_sha256",
}

def select_temporal_rejections(classifier_id, capture_id, records):
    """Select negative controls without observer labels; admissions are never sampled."""
    def rank(record):
        if (not isinstance(record, dict)
                or any(not isinstance(record.get(name), dict)
                       or type(record[name].get("video_frame_index")) is not int
                       for name in ("first", "last"))):
            raise ValueError("temporal rejection identity is malformed")
        identity = {
            "capture_id": capture_id,
            "classifier_id": classifier_id,
            "first_video_frame_index": record.get("first", {}).get("video_frame_index"),
            "last_video_frame_index": record.get("last", {}).get("video_frame_index"),
        }
        return (_canonical_sha256(identity), _canonical_sha256(record))
    return sorted(records, key=rank)[:TEMPORAL_CANDIDATE_SELECTION["maximum_rejections"]]


def temporal_selection_document(admitted_count, rejected_count, opaque_ids):
    if any(type(value) is not int or value < 0 for value in (admitted_count, rejected_count)):
        raise ValueError("temporal candidate denominators are invalid")
    return {
        "schema_version": 3,
        "kind": "blind_temporal_classifier_selection",
        "candidate_selection": deepcopy(TEMPORAL_CANDIDATE_SELECTION),
        "candidate_counts": {"admitted": admitted_count, "rejected": rejected_count},
        "selected_counts": {
            "admitted": admitted_count,
            "rejected": min(rejected_count, TEMPORAL_CANDIDATE_SELECTION["maximum_rejections"]),
        },
        "opaque_ids": opaque_ids,
    }


COMMON_TEMPORAL_IMPLEMENTATION_FILES = (
    "encounter_check.py",
    "encounter_sequence.py",
    "encounter_temporal.py",
    "encounter_configuration.py",
    "encounter_product_adapter.py",
    "encounter_product.py",
    "camera_artifacts.py",
    "camera_timing.py",
    "counter_check.py",
    "encounter_assessment.py",
    "visual_compare.py",
    "artifact_privacy.py",
)

CLASSIFIER_IMPLEMENTATION_FILES = {
    "v1-arrow-phase-edge-v2": (
        *COMMON_TEMPORAL_IMPLEMENTATION_FILES, "encounter_arrow_transition.py"),
    "v1-arrow-phase-edge-v3": (
        *COMMON_TEMPORAL_IMPLEMENTATION_FILES, "encounter_arrow_transition.py"),
    "v1-arrow-phase-edge-v4": (
        *COMMON_TEMPORAL_IMPLEMENTATION_FILES, "encounter_arrow_transition.py"),
    "v1-arrow-phase-edge-v5": (
        *COMMON_TEMPORAL_IMPLEMENTATION_FILES, "encounter_arrow_transition.py"),
    "v1-arrow-target-acquisition-v1": (
        *COMMON_TEMPORAL_IMPLEMENTATION_FILES, "encounter_arrow_acquisition.py"),
    "v1-stable-frequency-closed-context-v3": (
        *COMMON_TEMPORAL_IMPLEMENTATION_FILES,
        "encounter_frequency_context.py", "encounter_redraw_probe.py"),
    "v1-stable-frequency-intact-context-v1": (
        *COMMON_TEMPORAL_IMPLEMENTATION_FILES,
        "encounter_frequency_context.py", "encounter_redraw_probe.py"),
    "v1-main-bar-adjacent-redraw-v2": (
        *COMMON_TEMPORAL_IMPLEMENTATION_FILES,
        "encounter_bar_transition.py", "encounter_redraw_probe.py"),
    "v1-muted-badge-rising-fill-v2": (
        *COMMON_TEMPORAL_IMPLEMENTATION_FILES, "encounter_mute_redraw_transition.py",
        "encounter_redraw_probe.py"),
    "v1-unmute-stable-frequency-sweep-v2": (
        *COMMON_TEMPORAL_IMPLEMENTATION_FILES, "encounter_mute_redraw_transition.py",
        "encounter_redraw_probe.py"),
    "v1-secondary-closed-context-v2": (
        *COMMON_TEMPORAL_IMPLEMENTATION_FILES, "encounter_secondary_context.py"),
    "v1-secondary-closed-context-v3": (
        *COMMON_TEMPORAL_IMPLEMENTATION_FILES, "encounter_secondary_context.py"),
    "v1-secondary-text-optical-bridge-v1": (
        *COMMON_TEMPORAL_IMPLEMENTATION_FILES,
        "encounter_secondary_optical_bridge.py", "encounter_secondary_probe.py"),
    "secondary-closed-meter-corroboration-v1": COMMON_TEMPORAL_IMPLEMENTATION_FILES,
}

TEMPORAL_V2_OBSERVER_RUBRICS = {
    "v1-arrow-phase-edge-v5": {
        "raw_affected_fields": ["main_arrows"],
        "observer_eligibility_rule": (
            "COHERENT_SINGLE_DIRECTION_ON_OFF_EDGE with BOTH_CLEAR endpoints, "
            "NO extra-direction motion, and HIGH confidence"),
        "eligibility_detail": (
            "Let a and b be the first and last zero-based target clip-frame indices in "
            "target_run_clip_frame_indices. Use only the fixed left support pair [a-3,a-2] "
            "and right support pair [b+2,b+3]. Both frames in each pair must be clearly "
            "readable and agree on that side's direction set. The two sides must differ "
            "by one direction, and every consecutive frame in the full checked bracket "
            "[a-3,b+3], including the nearer frames a-1 and b+1, must show one coherent "
            "fill or clear of that direction. Every other direction stays unchanged, "
            "and confidence is HIGH. Do not search farther for clearer support or replace "
            "a fixed frame. Missing or unclear support requires uncertainty."),
        "field_guidance": {
            "left_endpoint_directions": (
                "Transcribe the complete visible direction set at the left endpoint a-2. "
                "[] is a valid clear endpoint when no direction is visibly active; use "
                "JSON null when the direction set is uncertain."),
            "right_endpoint_directions": (
                "Transcribe the complete visible direction set at the right endpoint b+2. "
                "[] is a valid clear endpoint when no direction is visibly active; use "
                "JSON null when the direction set is uncertain."),
            "center_class": (
                "Inspect every consecutive frame of [a-3,b+3], including a-1 and b+1. "
                "Broader clip context can identify directions but cannot supply replacement "
                "support. Do not assign the target run's frames either endpoint value."),
            "endpoint_support": (
                "BOTH_CLEAR requires the exact pairs [a-3,a-2] and [b+2,b+3] to be "
                "clearly readable and agree on direction presence. Brightness may change "
                "through the coherent fade; equal brightness is not required."),
            "extra_direction_motion": (
                "YES if any direction other than the one changing between endpoints also moves."),
            "confidence": "Use HIGH only when every required visual fact is clear.",
        },
        "literal_fields": (
            "left_endpoint_directions", "right_endpoint_directions", "center_class",
            "endpoint_support", "extra_direction_motion", "confidence"),
        "required_literals": {
            "center_class": "COHERENT_SINGLE_DIRECTION_ON_OFF_EDGE",
            "endpoint_support": "BOTH_CLEAR",
            "extra_direction_motion": "NO",
            "confidence": "HIGH",
        },
        "allowed_literals": {
            "center_class": (
                "COHERENT_SINGLE_DIRECTION_ON_OFF_EDGE",
                "NOT_COHERENT_SINGLE_DIRECTION_ON_OFF_EDGE",
                "VISUALLY_INDETERMINATE"),
            "endpoint_support": (
                "BOTH_CLEAR", "LEFT_UNCLEAR", "RIGHT_UNCLEAR", "BOTH_UNCLEAR",
                "INDETERMINATE"),
            "extra_direction_motion": ("NO", "YES", "INDETERMINATE"),
            "confidence": ("HIGH", "MEDIUM", "LOW"),
        },
    },
    "v1-arrow-target-acquisition-v1": {
        "raw_affected_fields": ["main_arrows"],
        "observer_eligibility_rule": (
            "BOTH_CLEAR endpoints, PRIOR_OR_PRIOR_PLUS_CURRENT to CURRENT direction, "
            "coherent changed-direction motion, "
            "EVERY_CLAIMED_FRAME_HAS_NONCURRENT_CHANGED_DIRECTION, "
            "no unchanged-direction motion, and HIGH confidence"),
        "eligibility_detail": (
            "Eligible only when the designated two stable clear endpoint frames on each side show a transition "
            "from a prior arrow phase, or that prior phase plus the current phase, to the current "
            "phase; every changed direction moves coherently, every claimed target frame retains "
            "at least one noncurrent changed direction, every unchanged direction stays still, "
            "and confidence is HIGH."),
        "field_guidance": {
            "left_endpoint_directions": (
                "Transcribe the complete visible direction set in left_support_clip_frame_indices. "
                "[] is a valid clear endpoint with no lit arrows; missing or unclear support is null."),
            "right_endpoint_directions": (
                "Transcribe the complete visible direction set in right_support_clip_frame_indices. "
                "[] is a valid clear endpoint with no lit arrows; missing or unclear support is null."),
            "endpoint_support": (
                "BOTH_CLEAR requires the designated two stable, mutually agreeing frames on each side. "
                "Use only left_support_clip_frame_indices and right_support_clip_frame_indices; "
                "do not substitute a later plateau or search farther for clearer endpoints. "
                "An empty support-index array means missing support, not an all-unlit endpoint. "
                "Missing or unclear designated support stays indeterminate."),
            "endpoint_relation": (
                "Use PRIOR_OR_PRIOR_PLUS_CURRENT_TO_CURRENT only when the left endpoint is a prior "
                "phase or its union with the right current phase, and the right endpoint is current."),
            "transition_class": (
                "Judge all changed arrow directions across exactly full_run_clip_frame_indices. "
                "The rest of the clip is orientation context, not a substitute local transition. "
                "These indices identify a dense interval of retained original video frames; "
                "they do not assert that a machine measured or accepted those frames."),
            "claimed_frame_acquisition": (
                "Use EVERY_CLAIMED_FRAME_HAS_NONCURRENT_CHANGED_DIRECTION only when each marked "
                "target frame visibly differs from the current endpoint in at least one changed "
                "direction."),
            "unchanged_direction_motion": (
                "YES if any arrow direction unchanged between endpoints moves visibly."),
            "confidence": "Use HIGH only when every required visual fact is clear.",
        },
        "literal_fields": (
            "left_endpoint_directions", "right_endpoint_directions", "endpoint_support",
            "endpoint_relation", "transition_class",
            "claimed_frame_acquisition", "unchanged_direction_motion", "confidence"),
        "required_literals": {
            "endpoint_support": "BOTH_CLEAR",
            "endpoint_relation": "PRIOR_OR_PRIOR_PLUS_CURRENT_TO_CURRENT",
            "transition_class": "COHERENT_CHANGED_DIRECTION_MOTION",
            "claimed_frame_acquisition": (
                "EVERY_CLAIMED_FRAME_HAS_NONCURRENT_CHANGED_DIRECTION"),
            "unchanged_direction_motion": "NO",
            "confidence": "HIGH",
        },
        "allowed_literals": {
            "endpoint_support": (
                "BOTH_CLEAR", "LEFT_UNCLEAR", "RIGHT_UNCLEAR", "BOTH_UNCLEAR",
                "INDETERMINATE"),
            "endpoint_relation": (
                "PRIOR_OR_PRIOR_PLUS_CURRENT_TO_CURRENT", "OTHER_ENDPOINT_RELATION",
                "INDETERMINATE"),
            "transition_class": (
                "COHERENT_CHANGED_DIRECTION_MOTION", "NONCOHERENT_CHANGED_DIRECTION_MOTION",
                "VISUALLY_INDETERMINATE"),
            "claimed_frame_acquisition": (
                "EVERY_CLAIMED_FRAME_HAS_NONCURRENT_CHANGED_DIRECTION",
                "A_CLAIMED_FRAME_IS_CURRENT_IN_ALL_CHANGED_DIRECTIONS",
                "VISUALLY_INDETERMINATE"),
            "unchanged_direction_motion": ("NO", "YES", "INDETERMINATE"),
            "confidence": ("HIGH", "MEDIUM", "LOW"),
        },
    },
    "v1-stable-frequency-intact-context-v1": {
        "raw_affected_fields": ["primary_frequency"],
        "observer_eligibility_rule": (
            "SAME_FREQUENCY_GLYPHS_THROUGHOUT with ALL_SEGMENTS_COMPLETE, BOTH_CLEAR endpoints, "
            "LEGAL_TARGET_CONTENT, and HIGH confidence"),
        "eligibility_detail": (
            "Eligible only when two clear same-frequency supports close each side, every frame "
            "between them preserves those same frequency glyphs with every illuminated segment "
            "complete, the content is a legal current target, and confidence is HIGH. Partial "
            "segments anywhere in the complete closing context are ineligible. Do not infer "
            "or record the machine branch."),
        "field_guidance": {
            "observed_frequency": (
                "Transcribe the exact five frequency digits and decimal visible in the context."),
            "frequency_glyph_relation": (
                "Judge the complete closed context and use SAME_FREQUENCY_GLYPHS_THROUGHOUT only "
                "when the same five frequency digits and decimal persist throughout."),
            "endpoint_support": (
                "BOTH_CLEAR requires two stable, mutually agreeing frequency frames on each side."),
            "segment_integrity": (
                "ALL_SEGMENTS_COMPLETE requires every illuminated segment to retain its full "
                "shape throughout the entire closing context, including both support pairs. "
                "A readable number with a partial or disappearing stroke does not qualify."),
            "target_content": (
                "LEGAL_TARGET_CONTENT requires the shown frequency to be one permitted current "
                "target; do not use a machine branch label."),
            "confidence": "Use HIGH only when every required visual fact is clear.",
        },
        "literal_fields": (
            "observed_frequency", "frequency_glyph_relation", "segment_integrity", "endpoint_support",
            "target_content", "confidence"),
        "required_literals": {
            "frequency_glyph_relation": "SAME_FREQUENCY_GLYPHS_THROUGHOUT",
            "segment_integrity": "ALL_SEGMENTS_COMPLETE",
            "endpoint_support": "BOTH_CLEAR",
            "target_content": "LEGAL_TARGET_CONTENT",
            "confidence": "HIGH",
        },
        "allowed_literals": {
            "frequency_glyph_relation": (
                "SAME_FREQUENCY_GLYPHS_THROUGHOUT", "FREQUENCY_GLYPHS_CHANGE",
                "VISUALLY_INDETERMINATE"),
            "segment_integrity": (
                "ALL_SEGMENTS_COMPLETE", "PARTIAL_OR_MISSING_SEGMENT", "INDETERMINATE"),
            "endpoint_support": (
                "BOTH_CLEAR", "LEFT_UNCLEAR", "RIGHT_UNCLEAR", "BOTH_UNCLEAR",
                "INDETERMINATE"),
            "target_content": (
                "LEGAL_TARGET_CONTENT", "NOT_LEGAL_TARGET_CONTENT", "INDETERMINATE"),
            "confidence": ("HIGH", "MEDIUM", "LOW"),
        },
    },
    "v1-secondary-closed-context-v3": {
        "raw_affected_fields": ["secondary"],
        "observer_eligibility_rule": (
            "SAME_CURRENT_CARD_CONTEXT with BOTH_CLEAR support pairs, coherent partial-meter "
            "redraw, EXACT_COUNT_CLOSURE, and HIGH confidence"),
        "eligibility_detail": (
            "Eligible only when both support pairs clearly show the same current one-or-two-card "
            "presentation, every marked refusal preserves that card identity through a coherent "
            "partial-meter redraw, the complete context uniquely closes each meter on the support "
            "count, and confidence is HIGH."),
        "field_guidance": {
            "observed_cards": (
                "Transcribe the complete one-or-two-card presentation in slot order, including "
                "band, frequency, direction, and bar count for each card."),
            "card_context": (
                "SAME_CURRENT_CARD_CONTEXT requires identical card count, slot, band, frequency, "
                "direction, and readable text throughout the complete marked context."),
            "support_pairs": (
                "BOTH_CLEAR requires two stable, mutually agreeing readable frames on each side."),
            "meter_redraw": (
                "COHERENT_PARTIAL_METER_REDRAW requires every marked refusal to preserve the card "
                "identity and show at least one partial strength cell compatible with the endpoints."),
            "count_closure": (
                "EXACT_COUNT_CLOSURE requires the complete context to leave exactly the support "
                "bar count possible for every card."),
            "confidence": "Use HIGH only when every required visual fact is clear.",
        },
        "literal_fields": (
            "observed_cards", "card_context", "support_pairs", "meter_redraw",
            "count_closure", "confidence"),
        "required_literals": {
            "card_context": "SAME_CURRENT_CARD_CONTEXT",
            "support_pairs": "BOTH_CLEAR",
            "meter_redraw": "COHERENT_PARTIAL_METER_REDRAW",
            "count_closure": "EXACT_COUNT_CLOSURE",
            "confidence": "HIGH",
        },
        "allowed_literals": {
            "card_context": (
                "SAME_CURRENT_CARD_CONTEXT", "CARD_CONTEXT_CHANGES",
                "VISUALLY_INDETERMINATE"),
            "support_pairs": (
                "BOTH_CLEAR", "LEFT_UNCLEAR", "RIGHT_UNCLEAR", "BOTH_UNCLEAR",
                "INDETERMINATE"),
            "meter_redraw": (
                "COHERENT_PARTIAL_METER_REDRAW", "INCOMPATIBLE_METER_CONTENT",
                "VISUALLY_INDETERMINATE"),
            "count_closure": (
                "EXACT_COUNT_CLOSURE", "COUNT_NOT_UNIQUELY_CLOSED", "INDETERMINATE"),
            "confidence": ("HIGH", "MEDIUM", "LOW"),
        },
    },
    "v1-secondary-text-optical-bridge-v1": {
        "raw_affected_fields": ["secondary"],
        "observer_eligibility_rule": (
            "SAME_COMPLETE_CARD_TEXT_THROUGHOUT with BOTH_CLEAR support pairs, "
            "CENTER_COMPLETE_TEXT_CLEAR, NO_DIRECTION_OR_METER_CHANGE, and HIGH confidence"),
        "eligibility_detail": (
            "Eligible only when two clear support frames on each side and the center frame show "
            "the same complete one-or-two-card text, the center text is complete and clear, "
            "direction and meter content do not change, and confidence is HIGH."),
        "field_guidance": {
            "support_cards": (
                "Transcribe the complete one-or-two-card presentation in support slot order, "
                "including band, frequency, direction, and bar count for each card."),
            "center_cards": (
                "Independently transcribe the complete center-frame card presentation in slot "
                "order, including band, frequency, direction, and bar count for each card."),
            "support_pairs": (
                "BOTH_CLEAR requires two stable, mutually agreeing readable frames on each side."),
            "center_text": (
                "CENTER_COMPLETE_TEXT_CLEAR requires the complete band and frequency text to be "
                "visibly clear in the marked center frame."),
            "card_text_relation": (
                "SAME_COMPLETE_CARD_TEXT_THROUGHOUT requires every support and center frame to "
                "show the identical complete text for every card."),
            "direction_or_meter_change": (
                "Use NO_DIRECTION_OR_METER_CHANGE only when direction and all meter cells remain "
                "visibly unchanged through the complete five-frame bracket."),
            "confidence": "Use HIGH only when every required visual fact is clear.",
        },
        "literal_fields": (
            "support_cards", "center_cards", "support_pairs", "center_text", "card_text_relation",
            "direction_or_meter_change", "confidence"),
        "required_literals": {
            "support_pairs": "BOTH_CLEAR",
            "center_text": "CENTER_COMPLETE_TEXT_CLEAR",
            "card_text_relation": "SAME_COMPLETE_CARD_TEXT_THROUGHOUT",
            "direction_or_meter_change": "NO_DIRECTION_OR_METER_CHANGE",
            "confidence": "HIGH",
        },
        "allowed_literals": {
            "support_pairs": (
                "BOTH_CLEAR", "LEFT_UNCLEAR", "RIGHT_UNCLEAR", "BOTH_UNCLEAR",
                "INDETERMINATE"),
            "center_text": (
                "CENTER_COMPLETE_TEXT_CLEAR", "CENTER_COMPLETE_TEXT_NOT_CLEAR",
                "VISUALLY_INDETERMINATE"),
            "card_text_relation": (
                "SAME_COMPLETE_CARD_TEXT_THROUGHOUT", "CARD_TEXT_CHANGES",
                "VISUALLY_INDETERMINATE"),
            "direction_or_meter_change": (
                "NO_DIRECTION_OR_METER_CHANGE", "DIRECTION_OR_METER_CHANGE",
                "VISUALLY_INDETERMINATE"),
            "confidence": ("HIGH", "MEDIUM", "LOW"),
        },
    },
    "v1-main-bar-adjacent-redraw-v2": {
        "raw_affected_fields": ["main_bars"],
        "observer_eligibility_rule": (
            "COHERENT_SINGLE_BOUNDARY_ADJACENT_REDRAW with BOTH_CLEAR endpoints, "
            "NO extra-cell motion, and HIGH confidence"),
        "eligibility_detail": (
            "Eligible only when both endpoint counts are clear integers from 0 through 6, "
            "differ by exactly one, direction agrees with the count change, one boundary cell "
            "redraws coherently, all other cells stay still, and confidence is HIGH."),
        "field_guidance": {
            "left_endpoint_bar_count": (
                "Count contiguous lit main-meter bars in the two clear frames before the run; "
                "use null when they cannot be resolved."),
            "right_endpoint_bar_count": (
                "Count contiguous lit main-meter bars in the two clear frames after the run; "
                "use null when they cannot be resolved."),
            "endpoint_support": (
                "BOTH_CLEAR requires two stable, mutually agreeing frames on each side."),
            "transition_class": (
                "Classify only motion within the target run between the stable endpoints."),
            "extra_cell_motion": (
                "YES if any bar cell other than the one adjacent boundary changes visibly."),
            "direction": "RISING means the right count is one higher; FALLING means one lower.",
            "confidence": "Use HIGH only when every required visual fact is clear.",
        },
        "literal_fields": (
            "left_endpoint_bar_count", "right_endpoint_bar_count", "endpoint_support",
            "transition_class", "extra_cell_motion", "direction", "confidence"),
        "required_literals": {
            "endpoint_support": "BOTH_CLEAR",
            "transition_class": "COHERENT_SINGLE_BOUNDARY_ADJACENT_REDRAW",
            "extra_cell_motion": "NO",
            "confidence": "HIGH",
        },
        "allowed_literals": {
            "endpoint_support": (
                "BOTH_CLEAR", "LEFT_UNCLEAR", "RIGHT_UNCLEAR", "BOTH_UNCLEAR",
                "INDETERMINATE"),
            "transition_class": (
                "COHERENT_SINGLE_BOUNDARY_ADJACENT_REDRAW",
                "MULTIPLE_OR_NONADJACENT_BAR_CHANGE", "SAME_COUNT_OR_ISOLATED_ARTIFACT",
                "VISUALLY_INDETERMINATE"),
            "extra_cell_motion": ("NO", "YES", "INDETERMINATE"),
            "direction": ("RISING", "FALLING", "INDETERMINATE"),
            "confidence": ("HIGH", "MEDIUM", "LOW"),
        },
    },
    "v1-muted-badge-rising-fill-v2": {
        "raw_affected_fields": ["muted_badge"],
        "observer_eligibility_rule": (
            "COHERENT_BADGE_RISING_FILL with BOTH_CLEAR endpoints, RISING direction, "
            "and HIGH confidence"),
        "eligibility_detail": (
            "Eligible only for a clear ABSENT-to-PRESENT badge change with two stable support "
            "frames on each side, a coherent rising fill, no outside badge content or geometry "
            "change, and HIGH confidence. Uniform palette or brightness recoloring caused by "
            "mute is allowed."),
        "field_guidance": {
            "left_endpoint_badge": "Judge the stable badge state before the target run.",
            "right_endpoint_badge": "Judge the stable badge state after the target run.",
            "endpoint_support": (
                "BOTH_CLEAR requires two stable, mutually agreeing frames on each side."),
            "transition_class": (
                "Judge whether the badge fills coherently through the intervening target run."),
            "direction": "RISING means ABSENT before and PRESENT after.",
            "outside_badge_content_change": (
                "NO permits uniform palette or brightness recoloring caused by mute. Use YES "
                "only when frequency, band, direction, bar geometry or count, or other visible "
                "content changes outside the muted-badge region."),
            "confidence": "Use HIGH only when every required visual fact is clear.",
        },
        "literal_fields": (
            "left_endpoint_badge", "right_endpoint_badge", "endpoint_support",
            "transition_class", "direction", "outside_badge_content_change", "confidence"),
        "required_literals": {
            "left_endpoint_badge": "ABSENT",
            "right_endpoint_badge": "PRESENT",
            "endpoint_support": "BOTH_CLEAR",
            "transition_class": "COHERENT_BADGE_RISING_FILL",
            "direction": "RISING",
            "outside_badge_content_change": "NO",
            "confidence": "HIGH",
        },
        "allowed_literals": {
            "left_endpoint_badge": ("ABSENT", "PRESENT", "INDETERMINATE"),
            "right_endpoint_badge": ("ABSENT", "PRESENT", "INDETERMINATE"),
            "endpoint_support": (
                "BOTH_CLEAR", "LEFT_UNCLEAR", "RIGHT_UNCLEAR", "BOTH_UNCLEAR",
                "INDETERMINATE"),
            "transition_class": (
                "COHERENT_BADGE_RISING_FILL", "BADGE_FALLING_OR_NONMONOTONE",
                "SAME_STATE_OR_ISOLATED_ARTIFACT", "VISUALLY_INDETERMINATE"),
            "direction": ("RISING", "FALLING", "INDETERMINATE"),
            "outside_badge_content_change": ("NO", "YES", "INDETERMINATE"),
            "confidence": ("HIGH", "MEDIUM", "LOW"),
        },
    },
    "v1-unmute-stable-frequency-sweep-v2": {
        "raw_affected_fields": ["primary_frequency"],
        "observer_eligibility_rule": (
            "COHERENT_STABLE_FREQUENCY_ILLUMINATION_SWEEP with BOTH_CLEAR endpoints, "
            "SAME_GLYPHS, and HIGH confidence"),
        "eligibility_detail": (
            "Eligible only when both endpoints show the same canonical DD.DDD frequency, two "
            "stable frames support each side, the identical glyph strokes brighten coherently "
            "from dim to bright without extra ink or filled holes, and confidence is HIGH."),
        "field_guidance": {
            "left_endpoint_frequency": (
                "Read the stable canonical DD.DDD value before the target run; use null if unclear."),
            "right_endpoint_frequency": (
                "Read the stable canonical DD.DDD value after the target run; use null if unclear."),
            "endpoint_support": (
                "BOTH_CLEAR requires two stable, mutually agreeing frames on each side."),
            "transition_class": (
                "Judge whether the same glyph strokes brighten coherently through the target run."),
            "glyph_relation": (
                "SAME_GLYPHS requires identical digit shapes and decimal placement at both ends."),
            "illumination_direction": "DIM_TO_BRIGHT means the retained strokes brighten overall.",
            "extra_ink_or_hole_fill": (
                "YES for any extra stroke, filled digit hole, or interrupted decimal evidence."),
            "confidence": "Use HIGH only when every required visual fact is clear.",
        },
        "literal_fields": (
            "left_endpoint_frequency", "right_endpoint_frequency", "endpoint_support",
            "transition_class", "glyph_relation", "illumination_direction",
            "extra_ink_or_hole_fill", "confidence"),
        "required_literals": {
            "endpoint_support": "BOTH_CLEAR",
            "transition_class": "COHERENT_STABLE_FREQUENCY_ILLUMINATION_SWEEP",
            "glyph_relation": "SAME_GLYPHS",
            "illumination_direction": "DIM_TO_BRIGHT",
            "extra_ink_or_hole_fill": "NO",
            "confidence": "HIGH",
        },
        "allowed_literals": {
            "endpoint_support": (
                "BOTH_CLEAR", "LEFT_UNCLEAR", "RIGHT_UNCLEAR", "BOTH_UNCLEAR",
                "INDETERMINATE"),
            "transition_class": (
                "COHERENT_STABLE_FREQUENCY_ILLUMINATION_SWEEP",
                "GLYPH_CHANGE_OR_EXTRA_STROKE", "SAME_BRIGHTNESS_OR_ISOLATED_ARTIFACT",
                "VISUALLY_INDETERMINATE"),
            "glyph_relation": ("SAME_GLYPHS", "DIFFERENT_GLYPHS", "INDETERMINATE"),
            "illumination_direction": (
                "DIM_TO_BRIGHT", "BRIGHT_TO_DIM", "NONMONOTONE", "INDETERMINATE"),
            "extra_ink_or_hole_fill": ("NO", "YES", "INDETERMINATE"),
            "confidence": ("HIGH", "MEDIUM", "LOW"),
        },
    },
}

_TEMPORAL_V2_REJECTION_CODES = {
    "v1-arrow-phase-edge-v5": {
        "UNCLOSED_RUN", "SOURCE_GAP", "ENDPOINT_SPAN", "UNSTABLE_ENDPOINT",
        "NOT_ONE_DIRECTION_PHASE_EDGE", "EXPECTATION_SIGNATURE", "EXTRA_DIRECTION_STATE",
        "INVALID_PROFILE", "ENDPOINT_SEPARATION", "PROJECTION_RANGE",
        "NORMALIZED_RESIDUAL", "MAXIMUM_BACKWARD_STEP", "TOTAL_BACKWARD_MOTION",
        "EXTRA_DIRECTION_MOTION",
    },
    "v1-arrow-target-acquisition-v1": {
        "UNCLOSED_RUN", "SOURCE_GAP", "ENDPOINT_SPAN", "EXPECTATION_SIGNATURE",
        "NOT_ACQUISITION_ENDPOINTS", "NO_PRODUCT_CLAIM", "NONCONTIGUOUS_PRODUCT_CLAIM",
        "TRANSITION_READING", "INVALID_PROFILE", "ENDPOINT_SEPARATION",
        "PROJECTION_RANGE", "NORMALIZED_RESIDUAL", "MAXIMUM_BACKWARD_STEP",
        "TOTAL_BACKWARD_MOTION", "UNCHANGED_DIRECTION_MOTION",
        "CLAIMED_FRAME_AT_CURRENT_ENDPOINT",
    },
    "v1-stable-frequency-intact-context-v1": {
        "RUN_SPAN", "UNCLOSED_RUN", "SOURCE_GAP", "SUPPORT_SPAN", "SUPPORT_VALUE",
        "SUPPORT_COMPARISON", "SUPPORT_GEOMETRY", "TARGET_MISMATCH", "CONTEXT_GEOMETRY",
        "PRODUCT_FIELD_SCOPE", "READER_REASON", "FREQUENCY_GEOMETRY",
        "NONCONTIGUOUS_PRODUCT_CLAIM",
    },
    "v1-secondary-closed-context-v3": {
        "EMPTY_EPISODE", "UNCLOSED_RUN", "SOURCE_GAP", "CONTEXT_SPAN",
        "SUPPORT_SPAN", "SUPPORT_VALUE", "SUPPORT_COMPARISON", "TARGET_MISMATCH",
        "TARGET_ACQUISITION_CONTEXT", "INTERIOR_CONTEXT", "PRODUCT_FIELD_SCOPE",
        "CARD_REDRAW_CONTEXT", "METER_COMPATIBILITY", "NONCONTIGUOUS_PRODUCT_CLAIM",
    },
    "v1-secondary-text-optical-bridge-v1": {
        "UNCLOSED_BRACKET", "SOURCE_GAP", "SUPPORT_SPAN", "SUPPORT_VALUE",
        "SUPPORT_COMPARISON", "PRODUCT_FIELD_SCOPE", "TARGET_MISMATCH",
        "TARGET_ACQUISITION_CONTEXT", "CARD_EVIDENCE", "OPTICAL_PROFILE",
        "OPTICAL_DIFFERENCE",
    },
    "v1-main-bar-adjacent-redraw-v2": {
        "UNCLOSED_RUN", "SOURCE_GAP", "ENDPOINT_SPAN", "UNSTABLE_ENDPOINT",
        "NOT_ADJACENT_COUNTS", "EXPECTATION_SIGNATURE", "NOT_BOUNDARY_ONLY",
        "UNCHANGED_CELL_MOTION", "ENDPOINT_SEPARATION", "PROJECTION_RANGE",
        "NORMALIZED_RESIDUAL", "MAXIMUM_BACKWARD_STEP", "TOTAL_BACKWARD_MOTION",
        "BOUNDARY_MEDIAN_BACKTRACK",
    },
    "v1-muted-badge-rising-fill-v2": {
        "EVENT_SCOPE", "UNCLOSED_RUN", "SOURCE_GAP", "SUPPORT_SPAN", "LEFT_SUPPORT", "RIGHT_SUPPORT",
        "PRODUCT_FIELD_SCOPE", "PROFILE_SCHEMA", "PROFILE_READER_MISMATCH",
        "ENDPOINT_SEPARATION", "PROGRESS_RANGE", "MAXIMUM_BACKWARD_STEP",
        "TOTAL_BACKWARD_MOTION",
    },
    "v1-unmute-stable-frequency-sweep-v2": {
        "EVENT_SCOPE", "UNCLOSED_RUN", "SOURCE_GAP", "SUPPORT_SPAN", "EXPECTED_FREQUENCY",
        "STABLE_FREQUENCY_SUPPORT", "NO_PRODUCT_CLAIM", "NONCONTIGUOUS_PRODUCT_CLAIM",
        "FREQUENCY_PROFILE", "ENDPOINT_SEPARATION", "PROGRESS_RANGE",
        "MAXIMUM_BACKWARD_STEP", "TOTAL_BACKWARD_MOTION",
    },
}

_BAR_RECORD_KEYS = {
    "event_id", "classifier_id", "classifier_spec_sha256", "status",
    "raw_affected_fields", "video_frame_indices", "first", "last", "left_support",
    "left_endpoint", "right_endpoint", "right_support", "endpoint_values",
    "changed_bar_index", "main_bar_expectation_signature", "endpoint_separation_rms",
    "projections", "normalized_residuals", "maximum_backward_step",
    "total_backward_motion", "boundary_medians",
    "maximum_boundary_median_backward_step", "unchanged_cell_profile_diameter_rms",
    "maximum_endpoint_span_ns", "verified_maximum_source_interval_ns", "profile_schema",
    "profile_boxes", "redraw_probe_method_version", "redraw_probe_sha256", "capture_id",
    "selection_manifest_sha256", "reader_method_version", "reader_sha256", "basis",
}
_ARROW_RECORD_KEYS = {
    "event_id", "classifier_id", "classifier_spec_sha256", "status",
    "raw_affected_fields", "video_frame_indices", "first", "last", "left_support",
    "left_endpoint", "right_endpoint", "right_support", "endpoint_values",
    "changed_direction", "arrow_expectation_signature", "endpoint_separation_rms",
    "profile_frame_indices", "projections", "normalized_residuals", "maximum_backward_step",
    "total_backward_motion", "extra_direction_profile_diameter_rms",
    "maximum_endpoint_span_ns", "verified_maximum_source_interval_ns", "profile_schema",
    "profile_reference_bounds", "capture_id", "selection_manifest_sha256",
    "reader_method_version", "reader_sha256", "basis",
}
_ARROW_ACQUISITION_RECORD_KEYS = {
    "event_id", "classifier_id", "classifier_spec_sha256", "status",
    "deadline_observation_semantics", "raw_affected_fields", "video_frame_indices",
    "first", "last",
    "full_transition_indices", "left_support", "right_support", "endpoint_values",
    "endpoint_phase_basis", "changed_directions", "arrow_expectation_signature",
    "direction_metrics", "claimed_frame_acquisition_proof",
    "unchanged_direction_profile_diameter_rms",
    "maximum_endpoint_span_ns", "support_search_frames_each_side",
    "verified_maximum_source_interval_ns", "profile_schema", "profile_reference_bounds",
    "capture_id", "selection_manifest_sha256", "reader_method_version", "reader_sha256",
    "basis",
}
_FREQUENCY_CONTEXT_RECORD_KEYS = {
    "event_id", "classifier_id", "classifier_spec_sha256", "status",
    "deadline_observation_semantics", "verification_closure_semantics", "branch",
    "raw_affected_fields",
    "video_frame_indices", "first", "last", "left_support", "right_support",
    "context_frame_indices", "context_observed_branches", "support_derived_frequency",
    "support_derived_digit_masks", "ambiguity_reason",
    "maximum_refusal_run_span_ns", "maximum_support_chain_span_ns",
    "verified_maximum_source_interval_ns", "capture_id", "selection_manifest_sha256",
    "reader_method_version", "reader_sha256", "redraw_probe_method_version",
    "redraw_probe_sha256", "event_signature", "basis",
}
_SECONDARY_CONTEXT_RECORD_KEYS = {
    "verification_closure_semantics", "auxiliary_closure_context_ns", "context_frame_indices",
    "event_id", "classifier_id", "classifier_spec_sha256", "status",
    "deadline_observation_semantics", "raw_affected_fields", "video_frame_indices",
    "first", "last", "full_context_indices", "context_refusal_indices",
    "interleaved_readable_indices", "context_first", "context_last", "left_support",
    "right_support", "current_presentation_established", "support_derived_secondary",
    "resolved_value", "partial_meter_evidence", "compatible_bar_intersections",
    "maximum_interleaved_readable_frames", "maximum_context_refusal_span_ns",
    "maximum_support_chain_span_ns", "maximum_support_chain_interval_ns",
    "verified_maximum_source_interval_ns", "capture_id", "selection_manifest_sha256",
    "reader_method_version", "reader_sha256", "event_signature", "basis",
}
_SECONDARY_OPTICAL_RECORD_KEYS = {
    "event_id", "classifier_id", "classifier_spec_sha256", "status",
    "deadline_observation_semantics", "raw_affected_fields", "video_frame_indices",
    "first", "last", "left_support", "right_support",
    "current_presentation_established", "deficient_slot", "raw_frequency_only_ocr",
    "support_derived_secondary", "resolved_value", "profile_schema",
    "profile_reference_bounds", "profile_sha256s", "profile_metrics", "profile_limits",
    "maximum_support_chain_span_ns", "maximum_support_chain_interval_ns",
    "verified_maximum_source_interval_ns", "capture_id", "selection_manifest_sha256",
    "reader_method_version", "reader_sha256", "secondary_probe_method_version",
    "secondary_probe_sha256", "event_signature", "basis",
}
_BADGE_RECORD_KEYS = {
    "event_id", "classifier_id", "classifier_spec_sha256", "status",
    "raw_affected_fields", "video_frame_indices", "first", "last", "left_support",
    "right_support", "event_signature", "endpoint_component_separation",
    "component_progress", "maximum_backward_step", "total_backward_motion_by_component",
    "maximum_support_chain_span_ns", "verified_maximum_source_interval_ns", "capture_id",
    "selection_manifest_sha256", "reader_method_version", "reader_sha256",
    "redraw_probe_method_version", "redraw_probe_sha256", "basis",
}
_FREQUENCY_RECORD_KEYS = _BADGE_RECORD_KEYS | {
    "full_field_run_indices", "full_field_run_first", "full_field_run_last",
    "expected_digit_masks",
}
_REJECTION_RECORD_KEYS = {
    "event_id", "classifier_id", "field", "code", "first", "last", "reason"}
_BRANCHED_REJECTION_RECORD_KEYS = _REJECTION_RECORD_KEYS | {"branch"}

REQUIRED_FAULT_CONTROLS = {
    "unchanged_original": ("MATCH", (), ()),
    "missing_primary": ("DIFFERENCE", ("primary_frequency",), ()),
    "wrong_strength": ("DIFFERENCE", ("main_bars",), ()),
    "wrong_direction": ("DIFFERENCE", ("main_arrows",), ()),
    "stale_primary": ("DIFFERENCE", ("primary_frequency",), ()),
    "missing_secondary": ("DIFFERENCE", ("secondary",), ()),
    "wrong_card_association": ("DIFFERENCE", ("secondary",), ()),
    "unreadable_camera": ("INCONCLUSIVE", (), FIELDS),
    "partial_frequency": ("INCONCLUSIVE", (), ("primary_frequency",)),
    "failure_with_unknown": (
        "DIFFERENCE", ("primary_frequency", "main_bars"), ("main_arrows",)),
}

_SHA256 = re.compile(r"[0-9a-f]{64}")
_GIT_SHA = re.compile(r"[0-9a-f]{40}")
_FIELD_STATUSES = {"AGREEMENT", "READER_REFUSAL", "REFERENCE_UNRESOLVED",
                   "WRONG_ASSERTION", "ASSERTION_WITHOUT_RESOLVED_REFERENCE"}


class QualificationError(ValueError):
    """A qualification bundle is missing, malformed, or does not qualify."""


def _require(condition: bool, reason: str) -> None:
    if not condition:
        raise QualificationError(reason)


def _pairs(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        _require(key not in result, f"duplicate JSON key: {key}")
        result[key] = value
    return result


def _read_json(path: Path) -> Any:
    def reject_nonfinite(value: str) -> None:
        raise QualificationError(f"qualification JSON contains non-finite number: {value}")

    try:
        return json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=_pairs,
                          parse_constant=reject_nonfinite)
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise QualificationError(f"qualification JSON is unreadable: {path.name}") from exc


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as stream:
            for block in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(block)
    except OSError as exc:
        raise QualificationError(f"qualification evidence is unreadable: {path.name}") from exc
    return digest.hexdigest()


def _canonical_sha256(value: Any) -> str:
    try:
        encoded = json.dumps(value, sort_keys=True, separators=(",", ":"),
                             allow_nan=False).encode("utf-8")
    except (TypeError, ValueError) as exc:
        raise QualificationError("frozen classifier record is not canonical JSON") from exc
    return hashlib.sha256(encoded).hexdigest()


def temporal_observer_clip_source_indices(
        target_indices: list[int], source_rows: dict[int, dict[str, int]],
        scope_indices: list[int] | None = None) -> list[int]:
    """Derive the observer window from authenticated time and optional local scope.

    Every supported record validator bounds its outermost required visual
    support chain to at most 300 ms and requires the target to lie inside that
    chain. Acquisition refusals may have more distant pre-gate supports; their
    explicit scope extends the same window without choosing different supports.
    """
    _require(isinstance(source_rows, dict) and bool(source_rows)
             and set(source_rows) == set(range(len(source_rows)))
             and all(isinstance(source_rows[index], dict)
                     and type(source_rows[index].get("capture_ns")) is int
                     for index in range(len(source_rows)))
             and all(source_rows[index + 1]["capture_ns"] >
                     source_rows[index]["capture_ns"]
                     for index in range(len(source_rows) - 1)),
             "observer source timing is invalid")
    _require(isinstance(target_indices, list) and bool(target_indices)
             and all(type(value) is int and value in source_rows
                     for value in target_indices)
             and all(right == left + 1
                     for left, right in zip(target_indices, target_indices[1:])),
             "observer target run is invalid")
    first_capture_ns = source_rows[target_indices[0]]["capture_ns"]
    last_capture_ns = source_rows[target_indices[-1]]["capture_ns"]
    lower = first_capture_ns - TEMPORAL_OBSERVER_CONTEXT_NS_EACH_SIDE
    upper = last_capture_ns + TEMPORAL_OBSERVER_CONTEXT_NS_EACH_SIDE
    indices = [index for index in range(len(source_rows))
               if lower <= source_rows[index]["capture_ns"] <= upper]
    if scope_indices is not None:
        _require(isinstance(scope_indices, list) and bool(scope_indices)
                 and all(type(value) is int and value in source_rows for value in scope_indices),
                 "observer local scope leaves retained source frames")
        indices = list(range(min(indices[0], min(scope_indices)),
                             max(indices[-1], max(scope_indices)) + 1))
    return indices


ACQUISITION_OBSERVER_SCOPE_FIELDS = {
    "full_run_clip_frame_indices", "left_support_clip_frame_indices",
    "right_support_clip_frame_indices",
}


def temporal_acquisition_observer_scope(
        item: dict[str, Any], observation: dict[str, Any] | None = None
) -> dict[str, list[int]]:
    """Validate public local geometry without reading a decision or hidden key."""
    source = item.get("clip_source_video_indices")
    target = item.get("target_run_clip_frame_indices")
    _require(isinstance(source, list) and bool(source)
             and all(type(value) is int and value >= 0 for value in source)
             and all(right == left + 1 for left, right in zip(source, source[1:])),
             "acquisition observer scope source mapping is invalid")
    for name in (*sorted(ACQUISITION_OBSERVER_SCOPE_FIELDS), "target_run_clip_frame_indices"):
        values = item.get(name)
        support = name in {"left_support_clip_frame_indices", "right_support_clip_frame_indices"}
        _require(isinstance(values, list)
                 and (len(values) in (0, 2) if support else bool(values))
                 and all(type(value) is int and 0 <= value < len(source) for value in values)
                 and all(right == left + 1 for left, right in zip(values, values[1:])),
                 f"acquisition observer scope is invalid: {name}")
    full = item["full_run_clip_frame_indices"]
    left = item["left_support_clip_frame_indices"]
    right = item["right_support_clip_frame_indices"]
    _require([source[index] for index in target] == item.get("target_run_video_indices")
             and all(index in full for index in target)
             and (left[-1] + 1 == full[0] if left else full[0] == target[0])
             and (right[0] - 1 == full[-1] if right else full[-1] == target[-1]),
             "acquisition observer scope does not bound its local target transition")
    if observation is not None:
        for side, indices in (("left", left), ("right", right)):
            if not indices:
                _require(observation.get(f"{side}_endpoint_directions") is None
                         and observation.get("endpoint_support") in {
                             f"{side.upper()}_UNCLEAR", "BOTH_UNCLEAR", "INDETERMINATE"},
                         "missing acquisition observer support must remain indeterminate")
    return {"full_transition_indices": [source[index] for index in full],
            "left_support": [source[index] for index in left],
            "right_support": [source[index] for index in right]}


def _validate_temporal_v2_acquisition_scope_binding(
        item: dict[str, Any], record: dict[str, Any], full_run: list[int]) -> None:
    scope = temporal_acquisition_observer_scope(item)
    _require(scope["full_transition_indices"] == record.get("full_transition_indices") == full_run,
             "acquisition observer full-run scope differs from the frozen record")
    for side in ("left_support", "right_support"):
        points = record.get(side)
        _require(isinstance(points, list) and len(points) in (0, 2)
                 and all(isinstance(point, dict) for point in points)
                 and scope[side] == [point.get("video_frame_index") for point in points],
                 f"acquisition observer {side} scope differs from the frozen record")


def temporal_v2_observer_instructions(classifier_id: str) -> str:
    rubric = TEMPORAL_V2_OBSERVER_RUBRICS.get(classifier_id)
    if not isinstance(rubric, dict):
        raise QualificationError(f"no generic temporal observer rubric for {classifier_id}")
    lines = [
        "Blind temporal qualification instructions v2\n"
        f"Classifier: {classifier_id}\n"
        f"Observer rubric SHA-256: {_canonical_sha256(rubric)}\n"
        "Before opening the hidden key or any machine output, inspect every opaque clip "
        "once in manifest order.\n"
        "Each manifest item maps its clip to the reserved run. clip_source_video_indices entry "
        "N is the original run video index shown at zero-based clip frame N. Every clip uses the "
        "same classifier-independent rule: all consecutive recorded frames within 300 ms before "
        "the target run's first frame through 300 ms after its last frame according to the "
        "authenticated capture timing, bounded only by the recording. "
        "target_run_clip_frame_indices maps "
        "the target run to target_run_video_indices. Use the visible surrounding frames to judge "
        "the rubric's endpoint, bracket, context, and transition requirements.\n"
        "Record exactly one listed literal for each field below; preserve spelling and case.\n"
        "After completing every observation, and still before receiving any hidden key or "
        "machine output, set observations.json blind_protocol to exactly: "
        "observations_completed_before_key_access=true, "
        "observer_received_machine_output=false, observer_received_hidden_key=false. "
        "Do not make that attestation if it is not true.\n"
    ]
    if classifier_id == "v1-arrow-target-acquisition-v1":
        lines.append(
            "For acquisition clips, deterministically extend that 300 ms context interval "
            "to include every designated full_run_clip_frame_indices, "
            "left_support_clip_frame_indices, and right_support_clip_frame_indices frame, "
            "retaining every original video ordinal between the outer boundaries. "
            "This same scope-union rule applies to every item. These three arrays use "
            "zero-based clip-frame indices and identify the exact local interval and support pairs.\n")
    for field in rubric["literal_fields"]:
        allowed = rubric["allowed_literals"].get(field)
        if allowed is None and field.endswith("_directions"):
            allowed_text = (
                "JSON array containing each visible direction once in front, side, rear order, "
                "or JSON null")
        elif allowed is None and field.endswith("_cards"):
            allowed_text = (
                "JSON array of one or two slot-ordered objects with exactly band, frequency, "
                "direction, and integer bars 0 through 6, or JSON null")
        elif allowed is None and "bar_count" in field:
            allowed_text = "integer 0 through 6, or JSON null"
        elif allowed is None and "frequency" in field:
            allowed_text = "canonical DD.DDD string, or JSON null"
        else:
            allowed_text = " | ".join(allowed)
        lines.append(
            f"- {field}: {rubric['field_guidance'][field]} Allowed: {allowed_text}.\n")
    lines.extend((
        f"Eligibility: {rubric['eligibility_detail']}\n",
        "Do not infer an unreadable endpoint or transition; use the rubric's explicit "
        "INDETERMINATE literal, or JSON null for a dynamic endpoint.\n",
    ))
    return "".join(lines)


def _digest(value: Any, name: str) -> str:
    _require(isinstance(value, str) and _SHA256.fullmatch(value) is not None,
             f"invalid {name} SHA-256")
    return value


def _evidence_file(root: Path, reference: Any, name: str) -> Path:
    _require(isinstance(reference, dict), f"missing {name} evidence reference")
    relative = reference.get("path")
    expected = _digest(reference.get("sha256"), name)
    _require(isinstance(relative, str) and relative and not Path(relative).is_absolute(),
             f"invalid {name} evidence path")
    candidate = (root / relative).resolve()
    try:
        candidate.relative_to(root.resolve())
    except ValueError as exc:
        raise QualificationError(f"{name} evidence escapes its bundle") from exc
    _require(candidate.is_file(), f"missing {name} evidence")
    _require(_sha256(candidate) == expected, f"{name} evidence hash mismatch")
    return candidate


def _evidence(root: Path, reference: Any, name: str) -> tuple[Path, Any]:
    candidate = _evidence_file(root, reference, name)
    return candidate, _read_json(candidate)


def _observe_image(path: Path, registration: Any) -> dict[str, Any]:
    """Run the exact imported reader on retained RGB pixels during verification."""
    try:
        from PIL import Image
        try:
            from .encounter_reader import observe
        except ImportError:
            from encounter_reader import observe
        with Image.open(path) as image:
            _require(image.mode == "RGB", f"qualification image is not retained RGB: {path.name}")
            return observe(image.tobytes(), *image.size, registration)
    except QualificationError:
        raise
    except (OSError, ValueError, TypeError) as exc:
        raise QualificationError(f"qualification image cannot be read: {path.name}") from exc


def _literal_observation(observed: Any) -> dict[str, Any]:
    _require(isinstance(observed, dict), "reader observation is malformed")
    return {"state": observed.get("state"), "value": deepcopy(observed.get("value"))}


def _partial_secondary_cards(observed: Any) -> list[dict[str, Any]] | None:
    """Return the literal partial-card data that can make a product disagreement."""
    _require(isinstance(observed, dict), "reader observation is malformed")
    partial = observed.get("partial_cards")
    if partial is None:
        return None
    _require(isinstance(partial, list) and len(partial) <= 2,
             "reader partial secondary cards are malformed")
    normalized = []
    for card in partial:
        _require(isinstance(card, dict)
                 and set(card) >= {"band", "frequency", "direction", "bars"},
                 "reader partial secondary card lacks associated fields")
        band = card.get("band")
        _require(band is None or isinstance(band, str)
                 and band.casefold() in ("x", "k", "ka"),
                 "reader partial secondary card has an invalid band")
        if band is not None:
            band = {"x": "X", "k": "K", "ka": "Ka"}[band.casefold()]
        frequency = card.get("frequency")
        _require(frequency is None or isinstance(frequency, str)
                 and re.fullmatch(r"(?:[0-9]{1,2}\.[0-9]{3}|--\.---)", frequency) is not None,
                 "reader partial secondary card has an invalid frequency")
        direction = card.get("direction")
        _require(direction is None or isinstance(direction, str)
                 and direction.casefold() in ("front", "side", "rear"),
                 "reader partial secondary card has an invalid direction")
        if direction is not None:
            direction = direction.casefold()
        bars = card.get("bars")
        _require(bars is None or type(bars) is int and 0 <= bars <= 6,
                 "reader partial secondary card has an invalid bar count")
        normalized.append({"band": band, "frequency": frequency,
                           "direction": direction, "bars": bars})
    return normalized


def _product_observation(field: str, observed: Any) -> dict[str, Any]:
    """Retain exactly the observation data consumed by the product comparator."""
    result = _literal_observation(observed)
    if field == "secondary":
        result["partial_cards"] = _partial_secondary_cards(observed)
    return result


def _partial_secondary_identities(observed: Any) -> list[tuple[str, str]]:
    """Return only literal identities that the no-secondary product check can assert."""
    _require(isinstance(observed, dict), "reader observation is malformed")
    if observed.get("state") not in ("unreadable", "ambiguous"):
        return []
    partial = _partial_secondary_cards(observed) or []
    return [(card["band"], card["frequency"]) for card in partial
            if card["band"] is not None and card["frequency"] is not None]


def _blind_secondary_identities(frame: Any) -> tuple[Counter[tuple[str, str]], int]:
    """Extract independently resolved identities and count unresolved blind cards."""
    if not isinstance(frame, dict) or frame.get("display_visibility") != "visible":
        return Counter(), 1
    cards = frame.get("cards")
    if not isinstance(cards, list) or frame.get("card_count") != len(cards):
        return Counter(), 1
    resolved: Counter[tuple[str, str]] = Counter()
    unresolved = 0
    for card in cards:
        band = card.get("band") if isinstance(card, dict) else None
        frequency = card.get("frequency") if isinstance(card, dict) else None
        if (band in ("X", "K", "Ka") and isinstance(frequency, str)
                and re.fullmatch(r"[0-9]{1,2}\.[0-9]{3}", frequency) is not None):
            resolved[(band, frequency)] += 1
        else:
            unresolved += 1
    return resolved, unresolved


def _normalized(field: str, value: Any) -> Any:
    if field in ("active_bands", "main_arrows") and isinstance(value, list):
        return sorted(str(item).casefold() for item in value)
    if field == "secondary" and isinstance(value, list):
        normalized = []
        for item in value:
            if not isinstance(item, dict):
                return value
            card = deepcopy(item)
            if isinstance(card.get("band"), str):
                card["band"] = card["band"].casefold()
            if isinstance(card.get("direction"), str):
                card["direction"] = card["direction"].casefold()
            normalized.append(json.dumps(card, sort_keys=True, separators=(",", ":")))
        return sorted(normalized)
    return value


def _derived_field_status(field: str, observed: Any, reference: Any) -> str:
    _require(isinstance(observed, dict) and isinstance(reference, dict),
             f"blind {field} label is malformed")
    resolved = {"readable", "absent"}
    expected_resolved = reference.get("state") in resolved
    observed_resolved = observed.get("state") in resolved
    if not expected_resolved:
        return ("ASSERTION_WITHOUT_RESOLVED_REFERENCE" if observed_resolved
                else "REFERENCE_UNRESOLVED")
    if not observed_resolved:
        return "READER_REFUSAL"
    return ("AGREEMENT" if _normalized(field, observed.get("value")) ==
            _normalized(field, reference.get("value")) else "WRONG_ASSERTION")


def _blind_secondary_reference(frame: Any) -> dict[str, Any]:
    """Translate the blind visual form into the reader's literal card contract."""
    unresolved = {"state": "unreadable", "value": None,
                  "reason": "blind secondary-card label is not fully resolved"}
    if not isinstance(frame, dict) or frame.get("display_visibility") != "visible":
        return unresolved
    cards = frame.get("cards")
    if not isinstance(cards, list) or frame.get("card_count") != len(cards):
        return unresolved
    directions = {"up": "front", "none": "side", "down": "rear"}
    value = []
    for card in cards:
        if (not isinstance(card, dict) or card.get("band") not in ("X", "K", "Ka")
                or not isinstance(card.get("frequency"), str)
                or re.fullmatch(r"[0-9]{1,2}\.[0-9]{3}", card["frequency"]) is None
                or card.get("direction") not in directions
                or type(card.get("bar_count")) is not int
                or not 0 <= card["bar_count"] <= 6):
            return unresolved
        count = card["bar_count"]
        if card.get("meter_cells") != ["lit"] * count + ["unlit"] * (6 - count):
            return unresolved
        value.append({"band": card["band"], "frequency": card["frequency"],
                      "direction": directions[card["direction"]], "bars": count})
    return {"state": "readable", "value": value}


def _visible_secondary_reanalysis_binding(document: dict[str, Any], hidden: Any,
                                           reader: dict[str, Any],
                                           implementation: dict[str, str],
                                           sealed_key_sha256: str) -> bool:
    """Validate an explicit reread of immutable labels made for an older reader."""
    binding = document.get("reader_reanalysis")
    if binding is None:
        return False
    source_implementation = hidden.get("implementation") if isinstance(hidden, dict) else None
    source_setup = (source_implementation.get("reader_setup")
                    if isinstance(source_implementation, dict) else None)
    _require(isinstance(source_setup, dict),
             "visible-secondary reanalysis has no source reader setup")
    expected = {
        "kind": "complete_exact_reader_reread",
        "source_sealed_key_sha256": sealed_key_sha256,
        "source_method_version": source_setup.get("method_version"),
        "source_reader_sha256": source_implementation.get("reader_sha256"),
        "current_method_version": reader.get("method_version"),
        "current_reader_sha256": implementation.get("encounter_reader.py"),
        "complete_source_set_reread": True,
    }
    _require(binding == expected,
             "visible-secondary reanalysis binding differs from source and current readers")
    _require((expected["source_method_version"], expected["source_reader_sha256"]) !=
             (expected["current_method_version"], expected["current_reader_sha256"]),
             "visible-secondary reanalysis does not identify a different reader")
    _digest(expected["source_reader_sha256"], "visible-secondary source reader")
    _digest(expected["current_reader_sha256"], "visible-secondary current reader")
    return True


def _validate_controls(controls: Any, evidence_root: Path) -> dict[str, Any]:
    _require(isinstance(controls, dict) and isinstance(controls.get("cases"), list),
             "field validation has no fault controls")
    expected = controls.get("expected")
    _require(isinstance(expected, dict), "fault controls have no retained expected state")
    registration = controls.get("registration")
    _require(isinstance(registration, dict) and registration.get("result") == "PASS",
             "fault controls have no qualified retained registration")
    cases = controls["cases"]
    by_name = {case.get("name"): case for case in cases if isinstance(case, dict)}
    _require(len(by_name) == len(cases), "fault controls have missing or duplicate names")
    _require(set(by_name) == set(REQUIRED_FAULT_CONTROLS),
             "fault controls do not cover the required product failures")
    demonstrated = 0
    image_hashes = set()
    for name, (desired, required_fields, required_unresolved) in REQUIRED_FAULT_CONTROLS.items():
        case = by_name[name]
        _require(case.get("desired_status") == desired,
                 f"fault control {name} has the wrong desired result")
        _require(tuple(case.get("required_differences", ())) == required_fields,
                 f"fault control {name} has the wrong required differences")
        _require(tuple(case.get("required_unresolved", ())) == required_unresolved,
                 f"fault control {name} has the wrong required unresolved fields")
        image = _evidence_file(evidence_root, case.get("image"), f"fault control {name} image")
        image_hash = _digest(case.get("image_sha256"), f"fault control {name} image")
        _require(_sha256(image) == image_hash and image_hash not in image_hashes,
                 f"fault control {name} image identity is inconsistent")
        image_hashes.add(image_hash)
        regenerated = _observe_image(image, registration)
        retained_fields = case.get("observed", {}).get("fields", case.get("observed"))
        regenerated_fields = regenerated.get("fields", regenerated)
        _require(isinstance(retained_fields, dict) and isinstance(regenerated_fields, dict)
                 and all(_product_observation(field, retained_fields.get(field)) ==
                         _product_observation(field, regenerated_fields.get(field))
                         for field in FIELDS),
                 f"fault control {name} observation differs from exact reader")
        try:
            derived = compare_sample(expected, case.get("observed"), role="held")
        except (ValueError, KeyError, TypeError) as exc:
            raise QualificationError(f"fault control {name} cannot be independently compared") from exc
        comparison = case.get("comparison")
        _require(comparison == derived,
                 f"fault control {name} retained comparison differs from independent comparison")
        checks = comparison["checks"]
        _require(set(checks) == set(FIELDS), f"fault control {name} does not check every field")
        actual = derived.get("status")
        passed = actual == desired and all(
            isinstance(checks[field], dict) and checks[field].get("status") == "DIFFERENCE"
            for field in required_fields)
        passed = passed and all(
            isinstance(checks[field], dict) and checks[field].get("status") == "UNRESOLVED"
            for field in required_unresolved)
        if desired == "MATCH":
            passed = passed and all(checks[field].get("status") == "MATCH" for field in FIELDS)
        if desired == "INCONCLUSIVE":
            passed = passed and not any(checks[field].get("status") == "DIFFERENCE" for field in FIELDS)
        _require(case.get("demonstrated") is passed,
                 f"fault control {name} has an inconsistent demonstrated flag")
        _require(passed, f"fault control {name} was not demonstrated")
        demonstrated += 1
    _require(controls.get("required") == len(REQUIRED_FAULT_CONTROLS)
             and controls.get("demonstrated") == demonstrated,
             "fault-control totals are inconsistent")
    return {"required": len(REQUIRED_FAULT_CONTROLS), "demonstrated": demonstrated}


def _validate_method_binding(document: Any, reader: dict[str, Any], camera: dict[str, Any],
                             implementation: dict[str, str], evidence_name: str) -> None:
    _require(document.get("reader") == reader,
             f"{evidence_name} used a different reader runtime")
    _require(document.get("camera") == camera,
             f"{evidence_name} used a different camera profile")
    method = document.get("method")
    _require(isinstance(method, dict) and method.get("method_version") == reader.get("method_version")
             and isinstance(method.get("files"), dict),
             f"{evidence_name} has no exact method freeze")
    for name in CORE_READER_FILES:
        _require(method["files"].get(name) == implementation.get(name),
                 f"{evidence_name} method differs: {name}")


def _validate_field_evidence(document: Any, reader: dict[str, Any], camera: dict[str, Any],
                             implementation: dict[str, str], evidence_root: Path) -> dict[str, Any]:
    _require(isinstance(document, dict) and document.get("schema_version") == 1
             and document.get("kind") == "reserved_independent_reader_validation",
             "unsupported field-validation evidence")
    _validate_method_binding(document, reader, camera, implementation, "field validation")
    source_artifacts = document.get("source_artifacts")
    required_sources = ("blind_manifest", "blind_observations", "selection")
    _require(isinstance(source_artifacts, dict) and set(required_sources) <= set(source_artifacts)
             and set(source_artifacts) <= {*required_sources, "primary_frequency_reference"},
             "field validation source artifacts are incomplete")
    source_documents = {}
    for name in required_sources:
        _, source_documents[name] = _evidence(
            evidence_root, source_artifacts[name], f"field validation {name}")
    source_hashes = document.get("method", {}).get("reference_source_sha256")
    expected_names = {"blind_manifest": "blind-manifest.json",
                      "blind_observations": "blind-observations.json",
                      "selection": "selection.json"}
    _require(isinstance(source_hashes, dict) and all(
        source_hashes.get(expected_names[name]) == source_artifacts[name].get("sha256")
        for name in required_sources), "field validation source hashes differ from method freeze")
    registration = source_documents["selection"].get("registration")
    _require(isinstance(registration, dict) and registration.get("result") == "PASS",
             "field validation has no qualified retained registration")
    adjudication = None
    if "primary_frequency_reference" in source_artifacts:
        from encounter_primary_frequency_reference import validate_reference
        supplement = _evidence_file(evidence_root, source_artifacts["primary_frequency_reference"],
                                    "primary frequency reference")
        try:
            adjudication = validate_reference(
                supplement,
                _evidence_file(evidence_root, source_artifacts["blind_manifest"], "original blind manifest"),
                _evidence_file(evidence_root, source_artifacts["blind_observations"], "original blind labels"),
                implementation, registration, _observe_image,
                reader_reanalysis=document.get("primary_frequency_reader_reanalysis"))
        except (OSError, ValueError, KeyError, TypeError) as exc:
            raise QualificationError(str(exc)) from exc
        _require(document.get("primary_frequency_adjudication") == adjudication["summary"],
                 "primary frequency adjudication summary differs from its independent evidence")
    else:
        _require("primary_frequency_adjudication" not in document
                 and "primary_frequency_reader_reanalysis" not in document,
                 "primary frequency adjudication lacks its independent reference")
    if reader.get("method_version", 0) >= 24:
        # Old image references cannot exercise a startup-calibrated branch.
        # These counts come from the verifier's exact reread above; every
        # assertion must still agree with its independent literal label.
        coverage = (adjudication or {}).get("summary", {}).get("calibrated_idle_coverage", {})
        startup_hashes = coverage.get("qualified_startup_image_sha256", [])
        _require(isinstance(startup_hashes, list) and len(set(startup_hashes)) >= 2,
                 "calibrated idle reader needs at least two qualified startup image sources")
        _require(type(coverage.get("calibrated_idle_acceptances")) is int
                 and coverage["calibrated_idle_acceptances"] >= 10,
                 "calibrated idle reader needs at least ten independently labelled actual acceptances")
        _require(type(coverage.get("residual_ink_refusals")) is int
                 and coverage["residual_ink_refusals"] >= 1,
                 "calibrated idle reader needs a demonstrated residual-ink refusal")
        if reader.get("method_version", 0) >= 25:
            _require(type(coverage.get("calibrated_numeric_decimal_acceptances")) is int
                     and coverage["calibrated_numeric_decimal_acceptances"] > 0,
                     "numeric decimal reader needs an independently labelled actual acceptance")
    frames = document.get("frames")
    _require(isinstance(frames, list) and len(frames) >= MINIMUM_BLIND_FRAMES,
             "too few blind field-validation frames")
    manifest_frames = source_documents["blind_manifest"].get("frames")
    blind_frames = source_documents["blind_observations"].get("frames")
    _require(isinstance(manifest_frames, list) and isinstance(blind_frames, list),
             "field validation source frame records are malformed")
    manifest_by_id = {item.get("frame_id"): item for item in manifest_frames
                      if isinstance(item, dict)}
    blind_by_id = {item.get("frame_id"): item for item in blind_frames if isinstance(item, dict)}
    _require(len(manifest_by_id) == len(manifest_frames)
             and len(blind_by_id) == len(blind_frames)
             and set(manifest_by_id) == set(blind_by_id),
             "field validation source frame identities differ")
    frame_ids, image_hashes = set(), set()
    totals: Counter[str] = Counter()
    per_field = {field: Counter() for field in FIELDS}
    empty_secondary_presence_controls = 0
    for frame in frames:
        _require(isinstance(frame, dict) and isinstance(frame.get("frame_id"), str),
                 "field-validation frame is malformed")
        image_hash = _digest(frame.get("image_sha256"), "field-validation image")
        _require(frame["frame_id"] not in frame_ids and image_hash not in image_hashes,
                 "field-validation frames are duplicated")
        frame_ids.add(frame["frame_id"])
        image_hashes.add(image_hash)
        source_manifest = manifest_by_id.get(frame["frame_id"])
        source_label = blind_by_id.get(frame["frame_id"])
        _require(isinstance(source_manifest, dict) and isinstance(source_label, dict)
                 and source_manifest.get("image_sha256") == image_hash
                 and source_label.get("image_sha256") == image_hash,
                 "field-validation frame differs from its blind source")
        image_path = _evidence_file(evidence_root, frame.get("image"),
                                    f"field-validation image {frame['frame_id']}")
        _require(_sha256(image_path) == image_hash,
                 "field-validation retained image identity differs")
        regenerated = _observe_image(image_path, registration)
        checks = frame.get("checks")
        _require(isinstance(checks, list), "field-validation checks are malformed")
        by_field = {check.get("field"): check for check in checks if isinstance(check, dict)}
        _require(len(by_field) == len(checks) and set(by_field) == set(FIELDS),
                 "field-validation frame does not label every field exactly once")
        for field in FIELDS:
            check = by_field[field]
            _require(_product_observation(field, check.get("observed")) ==
                     _product_observation(field, regenerated.get(field)),
                     f"field-validation observation differs from exact reader for {field}")
            expected_reference = source_label.get(field)
            if field == "primary_frequency" and adjudication and frame["frame_id"] in adjudication["overrides"]:
                _require(check.get("original_reference") == expected_reference,
                         "primary frequency correction did not preserve the original label")
                expected_reference = adjudication["overrides"][frame["frame_id"]]
            _require(check.get("reference") == expected_reference,
                     f"field-validation blind source label differs for {field}")
            status = check.get("status")
            _require(status in _FIELD_STATUSES, f"invalid blind status for {field}")
            _require(status == _derived_field_status(field, check.get("observed"), check.get("reference")),
                     f"blind status does not follow its literal labels for {field}")
            totals[status] += 1
            per_field[field][status] += 1
            if (field == "secondary" and isinstance(check.get("reference"), dict)
                    and check["reference"].get("state") == "readable"
                    and check["reference"].get("value") == []):
                _require(not _partial_secondary_identities(check.get("observed")),
                         "reader asserted a partial secondary identity on a blind empty control")
                empty_secondary_presence_controls += 1
    _require(document.get("unique_original_frames") == len(frames)
             and document.get("required_field_labels") == len(frames) * len(FIELDS),
             "field-validation denominators are inconsistent")
    _require(frame_ids == set(manifest_by_id),
             "field validation does not include its complete reserved source set")
    _require(document.get("counts") == dict(totals)
             and document.get("fields") == {field: dict(per_field[field]) for field in FIELDS},
             "field-validation totals do not match its frame records")
    _require(totals["WRONG_ASSERTION"] == 0,
             "blind field validation contains a wrong reader assertion")
    _require(totals["ASSERTION_WITHOUT_RESOLVED_REFERENCE"] == 0,
             "reader asserted content without a resolved blind reference")
    for field in FIELDS:
        _require(per_field[field]["AGREEMENT"] >= MINIMUM_AGREEMENTS_PER_FIELD,
                 f"too few blind agreements for {field}")
    _require(empty_secondary_presence_controls >= MINIMUM_EMPTY_SECONDARY_PRESENCE_CONTROLS,
             "too few blind empty-secondary presence controls")
    return {"unique_original_frames": len(frames), "required_field_labels": len(frames) * len(FIELDS),
            "counts": dict(totals), "agreements_by_field": {
                field: per_field[field]["AGREEMENT"] for field in FIELDS},
            "empty_secondary_presence_controls": empty_secondary_presence_controls,
            **({"primary_frequency_adjudication": adjudication["summary"]} if adjudication else {})}


def _validate_visible_secondary_evidence(document: Any, reader: dict[str, Any],
                                         camera: dict[str, Any], implementation: dict[str, str],
                                         evidence_root: Path) -> dict[str, Any]:
    _require(isinstance(document, dict) and document.get("schema_version") == 1
             and document.get("kind") == "independent_visible_secondary_validation",
             "unsupported visible-secondary validation evidence")
    _validate_method_binding(document, reader, camera, implementation,
                             "visible-secondary validation")
    protocol = document.get("blind_protocol")
    _require(isinstance(protocol, dict)
             and protocol.get("labels_completed_before_key_access") is True
             and protocol.get("observer_received_machine_output") is False
             and protocol.get("observer_received_hidden_key") is False,
             "visible-secondary validation was not independently blinded")
    source = document.get("source")
    _require(isinstance(source, dict), "visible-secondary validation has no source provenance")
    source_artifacts = document.get("source_artifacts")
    _require(isinstance(source_artifacts, dict)
             and set(source_artifacts) == set(VISIBLE_SECONDARY_SOURCE_NAMES),
             "visible-secondary source artifacts are incomplete")
    source_documents = {}
    for name in VISIBLE_SECONDARY_SOURCE_NAMES:
        _, source_documents[name] = _evidence(
            evidence_root, source_artifacts[name], f"visible-secondary {name}")
    for field, name in (("packet_manifest_sha256", "packet_manifest"),
                        ("blind_labels_sha256", "blind_labels"),
                        ("sealed_key_sha256", "sealed_key")):
        _require(_digest(source.get(field), f"visible-secondary {field}") ==
                 source_artifacts[name].get("sha256"),
                 f"visible-secondary {field} differs from retained source")
    packet_ids = {source_documents[name].get("packet_id") for name in
                  ("packet_manifest", "blind_labels", "sealed_key")}
    _require(len(packet_ids) == 1 and None not in packet_ids,
             "visible-secondary source packet identities differ")
    registration = document.get("registration")
    _require(isinstance(registration, dict) and registration.get("result") == "PASS",
             "visible-secondary validation has no qualified retained registration")

    manifest = source_documents["packet_manifest"]
    labels = source_documents["blind_labels"]
    hidden = source_documents["sealed_key"]
    image_integrity = manifest.get("image_integrity")
    _require(isinstance(image_integrity, dict),
             "visible-secondary packet has no image integrity map")

    def flatten_labels() -> dict[str, Any]:
        result = {}
        sequences = labels.get("ordered_sequences")
        singles = labels.get("single_frame_items")
        _require(isinstance(sequences, list) and isinstance(singles, list),
                 "visible-secondary blind labels are malformed")
        for sequence in sequences:
            _require(isinstance(sequence, dict) and isinstance(sequence.get("frames"), list),
                     "visible-secondary blind sequence is malformed")
            for frame in sequence["frames"]:
                _require(isinstance(frame, dict) and isinstance(frame.get("image"), str)
                         and frame["image"] not in result,
                         "visible-secondary blind image is missing or duplicated")
                result[frame["image"]] = frame
        for single in singles:
            frame = single.get("frame") if isinstance(single, dict) else None
            _require(isinstance(frame, dict) and isinstance(frame.get("image"), str)
                     and frame["image"] not in result,
                     "visible-secondary blind single image is missing or duplicated")
            result[frame["image"]] = frame
        return result

    def flatten_hidden() -> dict[str, Any]:
        result = {}
        sequences = hidden.get("sequences")
        singles = hidden.get("single_frame_controls")
        _require(isinstance(sequences, list) and isinstance(singles, list),
                 "visible-secondary sealed key is malformed")
        for sequence in sequences:
            _require(isinstance(sequence, dict) and isinstance(sequence.get("frames"), list),
                     "visible-secondary hidden sequence is malformed")
            for frame in sequence["frames"]:
                _require(isinstance(frame, dict) and isinstance(frame.get("image"), str)
                         and frame["image"] not in result,
                         "visible-secondary hidden image is missing or duplicated")
                result[frame["image"]] = frame.get("machine_secondary")
        for single in singles:
            _require(isinstance(single, dict) and isinstance(single.get("image"), str)
                     and single["image"] not in result,
                     "visible-secondary hidden single image is missing or duplicated")
            result[single["image"]] = single.get("machine_secondary")
        return result

    blind_by_image = flatten_labels()
    hidden_by_image = flatten_hidden()
    _require(set(image_integrity) == set(blind_by_image) == set(hidden_by_image),
             "visible-secondary source image sets differ")
    is_reanalysis = _visible_secondary_reanalysis_binding(
        document, hidden, reader, implementation, source_artifacts["sealed_key"]["sha256"])
    adjudication = source_documents["adjudication"]
    adjudicated_items = adjudication.get("items") if isinstance(adjudication, dict) else None
    _require(isinstance(adjudicated_items, list),
             "visible-secondary adjudication is malformed")
    adjudicated_by_image = {item.get("source_image"): item for item in adjudicated_items
                            if isinstance(item, dict)}
    _require(len(adjudicated_by_image) == len(adjudicated_items),
             "visible-secondary adjudication has duplicate images")
    items = document.get("items")
    _require(isinstance(items, list) and len(items) >= MINIMUM_NONEMPTY_SECONDARY_AGREEMENTS,
             "too few blind visible-secondary frames")
    item_ids, image_hashes, changed_references = set(), set(), set()
    counts: Counter[str] = Counter()
    partial_identity_agreement_frames = 0
    partial_identity_assertions = 0
    for item in items:
        _require(isinstance(item, dict) and isinstance(item.get("item_id"), str),
                 "visible-secondary item is malformed")
        image_hash = _digest(item.get("image_sha256"), "visible-secondary image")
        _require(item["item_id"] not in item_ids and image_hash not in image_hashes,
                 "visible-secondary frames are duplicated")
        item_ids.add(item["item_id"])
        image_hashes.add(image_hash)
        source_image = item.get("source_image")
        _require(item["item_id"] == source_image and source_image in image_integrity,
                 "visible-secondary item is not bound to its source image")
        integrity = image_integrity[source_image]
        _require(isinstance(integrity, dict) and integrity.get("sha256") == image_hash,
                 "visible-secondary source image hash differs")
        image_path = _evidence_file(evidence_root, item.get("image"),
                                    f"visible-secondary image {source_image}")
        _require(_sha256(image_path) == image_hash,
                 "visible-secondary retained image identity differs")
        _require(item.get("blind_label") == blind_by_image[source_image],
                 "visible-secondary retained blind label differs")
        if is_reanalysis:
            _product_observation("secondary", hidden_by_image[source_image])
        else:
            _require(item.get("observed") == hidden_by_image[source_image],
                     "visible-secondary retained reader observation differs")
        regenerated = _observe_image(image_path, registration).get("secondary")
        _require(_product_observation("secondary", item.get("observed")) ==
                 _product_observation("secondary", regenerated),
                 "visible-secondary observation differs from exact reader")
        asserted_identities = Counter(_partial_secondary_identities(item.get("observed")))
        if asserted_identities:
            blind_identities, unresolved_blind_cards = _blind_secondary_identities(
                blind_by_image[source_image])
            unmatched = asserted_identities - blind_identities
            if unmatched:
                _require(unresolved_blind_cards == 0,
                         "reader asserted a partial secondary identity without a resolved blind reference")
                _require(False, "blind visible-secondary validation contains a wrong partial identity")
            partial_identity_agreement_frames += 1
            partial_identity_assertions += sum(asserted_identities.values())
        primary_reference = _blind_secondary_reference(blind_by_image[source_image])
        _require(item.get("primary_reference") == primary_reference,
                 "visible-secondary primary reference was not independently derived")
        reference = item.get("reference")
        if reference != primary_reference:
            changed_references.add(source_image)
            audit = adjudicated_by_image.get(source_image)
            _require(isinstance(audit, dict)
                     and audit.get("original_reference") == primary_reference
                     and audit.get("adjudicated_reference") == reference
                     and isinstance(audit.get("basis"), str) and audit["basis"],
                     "visible-secondary changed reference lacks retained adjudication")
        status = item.get("status")
        _require(status in _FIELD_STATUSES, "invalid blind visible-secondary status")
        observed = item.get("observed")
        _require(status == _derived_field_status("secondary", observed, reference),
                 "visible-secondary status does not follow its literal labels")
        counts[status] += 1
    _require(item_ids == set(image_integrity),
             "visible-secondary validation does not include its complete blind packet")
    _require(changed_references == set(adjudicated_by_image),
             "visible-secondary adjudication set differs from changed references")
    _require(document.get("unique_original_frames") == len(items)
             and document.get("counts") == dict(counts),
             "visible-secondary denominators are inconsistent")
    _require(counts["WRONG_ASSERTION"] == 0,
             "blind visible-secondary validation contains a wrong reader assertion")
    _require(counts["ASSERTION_WITHOUT_RESOLVED_REFERENCE"] == 0,
             "reader asserted visible-secondary content without a resolved blind reference")
    nonempty_agreements = sum(
        item.get("status") == "AGREEMENT"
        and isinstance(item.get("reference"), dict)
        and isinstance(item["reference"].get("value"), list)
        and bool(item["reference"]["value"]) for item in items)
    _require(nonempty_agreements >= MINIMUM_NONEMPTY_SECONDARY_AGREEMENTS,
             "too few blind agreements on visible secondary cards")
    _require(partial_identity_agreement_frames >= MINIMUM_PARTIAL_SECONDARY_IDENTITY_AGREEMENTS,
             "too few blind agreements on partial secondary identities")
    supplement_summary = None
    if "secondary_reference" in document:
        from encounter_secondary_reference import validate_reference
        supplement = _evidence_file(evidence_root, document["secondary_reference"], "secondary reference")
        try:
            supplement_summary = validate_reference(
                supplement, implementation, _observe_image, camera=camera,
                reader_reanalysis=document.get("secondary_reference_reanalysis"))
        except (OSError, ValueError, TypeError, KeyError) as exc:
            raise QualificationError(str(exc)) from exc
        _require(document.get("secondary_reference_summary") == supplement_summary,
                 "secondary reference summary differs from complete exact reread")
    else:
        _require("secondary_reference_summary" not in document
                 and "secondary_reference_reanalysis" not in document,
                 "secondary reference summary lacks its independent originals")
    if reader.get("method_version", 0) >= 25:
        _require(supplement_summary is not None and supplement_summary["split_text_agreements"] > 0,
                 "secondary reference does not exercise the split-text reader against independent originals")
    if reader.get("method_version", 0) >= 26:
        _require(supplement_summary is not None and supplement_summary["band_pixel_agreements"] > 0,
                 "secondary reference does not exercise the complete-band reader against independent originals")
    result = {"unique_original_frames": len(items), "counts": dict(counts),
            "nonempty_secondary_agreements": nonempty_agreements,
            "partial_secondary_identity_agreement_frames": partial_identity_agreement_frames,
            "partial_secondary_identity_assertions": partial_identity_assertions,
            "reader_reanalysis": is_reanalysis}
    if supplement_summary is not None:
        result["secondary_reference"] = supplement_summary
    return result


def _validate_fault_evidence(document: Any, reader: dict[str, Any], camera: dict[str, Any],
                             implementation: dict[str, str], evidence_root: Path) -> dict[str, Any]:
    _require(isinstance(document, dict) and document.get("schema_version") == 1
             and document.get("kind") == "encounter_reader_fault_controls",
             "unsupported fault-control evidence")
    _validate_method_binding(document, reader, camera, implementation, "fault controls")
    return _validate_controls(document, evidence_root)


_OBSERVER_DIRECTION_ORDER = ("front", "side", "rear")


def _valid_observer_direction_set(value: Any) -> bool:
    return (isinstance(value, list)
            and value == [name for name in _OBSERVER_DIRECTION_ORDER if name in value]
            and len(value) == len(set(value)))


def _valid_observer_cards(value: Any) -> bool:
    return (isinstance(value, list) and 1 <= len(value) <= 2
            and all(isinstance(card, dict)
                    and set(card) == {"band", "frequency", "direction", "bars"}
                    and card.get("band") in {"X", "K", "Ka"}
                    and isinstance(card.get("frequency"), str)
                    and re.fullmatch(r"[0-9]{2}\.[0-9]{3}", card["frequency"]) is not None
                    and card.get("direction") in _OBSERVER_DIRECTION_ORDER
                    and type(card.get("bars")) is int and 0 <= card["bars"] <= 6
                    for card in value))


def _temporal_v2_observer_result(classifier_id: str, observation: Any) -> tuple[dict[str, Any], bool]:
    rubric = TEMPORAL_V2_OBSERVER_RUBRICS.get(classifier_id)
    _require(isinstance(rubric, dict),
             f"no generic temporal observer rubric for {classifier_id}")
    fields = rubric["literal_fields"]
    _require(isinstance(observation, dict)
             and set(observation) == {"opaque_id", *fields},
             f"temporal observer literal is malformed for {classifier_id}")
    literal = {name: deepcopy(observation[name]) for name in fields}
    for name, allowed in rubric["allowed_literals"].items():
        _require(literal[name] in allowed,
                 f"temporal observer literal {name} is invalid for {classifier_id}")

    if classifier_id in {"v1-arrow-phase-edge-v5", "v1-arrow-target-acquisition-v1"}:
        endpoints = (literal["left_endpoint_directions"],
                     literal["right_endpoint_directions"])
        _require(all(value is None or _valid_observer_direction_set(value)
                     for value in endpoints),
                 f"temporal observer arrow endpoints are invalid for {classifier_id}")
        relationship = (all(isinstance(value, list) for value in endpoints)
                        and (len(set(endpoints[0]) ^ set(endpoints[1])) == 1
                             if classifier_id == "v1-arrow-phase-edge-v5"
                             else endpoints[0] != endpoints[1]))
    elif classifier_id == "v1-stable-frequency-intact-context-v1":
        frequency = literal["observed_frequency"]
        _require(frequency is None or (isinstance(frequency, str)
                                       and re.fullmatch(r"[0-9]{2}\.[0-9]{3}", frequency)),
                 f"temporal observer frequency is invalid for {classifier_id}")
        relationship = frequency is not None
    elif classifier_id == "v1-secondary-closed-context-v3":
        cards = literal["observed_cards"]
        _require(cards is None or _valid_observer_cards(cards),
                 f"temporal observer cards are invalid for {classifier_id}")
        relationship = cards is not None
    elif classifier_id == "v1-secondary-text-optical-bridge-v1":
        support_cards, center_cards = literal["support_cards"], literal["center_cards"]
        _require(all(value is None or _valid_observer_cards(value)
                     for value in (support_cards, center_cards)),
                 f"temporal observer cards are invalid for {classifier_id}")
        relationship = (support_cards is not None and support_cards == center_cards)
    elif classifier_id == "v1-main-bar-adjacent-redraw-v2":
        counts = (literal["left_endpoint_bar_count"],
                  literal["right_endpoint_bar_count"])
        _require(all(value is None or (type(value) is int and 0 <= value <= 6)
                     for value in counts),
                 f"temporal observer bar endpoints are invalid for {classifier_id}")
        relationship = (all(type(value) is int for value in counts)
                        and abs(counts[1] - counts[0]) == 1
                        and literal["direction"] ==
                            ("RISING" if counts[1] > counts[0] else "FALLING"))
    elif classifier_id == "v1-muted-badge-rising-fill-v2":
        relationship = True
    elif classifier_id == "v1-unmute-stable-frequency-sweep-v2":
        frequencies = (literal["left_endpoint_frequency"],
                       literal["right_endpoint_frequency"])
        _require(all(value is None or (isinstance(value, str)
                                       and re.fullmatch(r"[0-9]{2}\.[0-9]{3}", value))
                     for value in frequencies),
                 f"temporal observer frequency endpoints are invalid for {classifier_id}")
        relationship = (frequencies[0] is not None
                        and frequencies[0] == frequencies[1])
    else:  # Guarded by the rubric lookup, retained for fail-closed future additions.
        raise QualificationError(f"unsupported generic temporal classifier: {classifier_id}")

    eligible = relationship and all(
        literal.get(name) == expected
        for name, expected in rubric["required_literals"].items())
    return literal, eligible


def _temporal_v2_observer_ground_truth(
        classifier_id: str, observation: Any) -> tuple[dict[str, Any], str]:
    """Return the observer literal and its independently scoreable class.

    A rejection only establishes a true negative when every visual fact needed
    to reject the classifier claim is clear. Unclear endpoints, unresolved
    literal values, or less-than-HIGH confidence remain unscorable.
    """
    literal, eligible = _temporal_v2_observer_result(classifier_id, observation)
    if eligible:
        return literal, "ELIGIBLE"
    unresolved = any(
        value is None
        or (isinstance(value, str)
            and ("INDETERMINATE" in value or "UNCLEAR" in value))
        for value in literal.values())
    definite_negative = (
        literal.get("endpoint_support", literal.get("support_pairs")) == "BOTH_CLEAR"
        and literal.get("confidence") == "HIGH"
        and not unresolved
    )
    return literal, ("DEFINITE_NEGATIVE" if definite_negative else "INDETERMINATE")


def _temporal_v2_claim_matches_record(classifier_id: str, spec_sha256: str,
                                      literal: dict[str, Any], record: dict[str, Any]) -> bool:
    if (record.get("classifier_id") != classifier_id
            or record.get("classifier_spec_sha256") != spec_sha256
            or record.get("status") != "QUALIFIED_CAPTURE_TRANSITION"
            or record.get("raw_affected_fields") !=
                TEMPORAL_V2_OBSERVER_RUBRICS[classifier_id]["raw_affected_fields"]):
        return False
    if classifier_id == "v1-arrow-phase-edge-v5":
        endpoints = record.get("endpoint_values")
        observed_endpoints = [literal.get("left_endpoint_directions"),
                              literal.get("right_endpoint_directions")]
        return (isinstance(endpoints, list) and len(endpoints) == 2
                and all(isinstance(value, list) for value in endpoints)
                and all(isinstance(value, list) for value in observed_endpoints)
                and all(set(machine) == set(observed) for machine, observed in zip(
                    endpoints, observed_endpoints))
                and len(set(endpoints[0]) ^ set(endpoints[1])) == 1)
    if classifier_id == "v1-arrow-target-acquisition-v1":
        endpoints = record.get("endpoint_values")
        observed_endpoints = [literal.get("left_endpoint_directions"),
                              literal.get("right_endpoint_directions")]
        phase = record.get("endpoint_phase_basis")
        changed = record.get("changed_directions")
        required_literals = TEMPORAL_V2_OBSERVER_RUBRICS[classifier_id][
            "required_literals"]
        return (all(literal.get(name) == expected
                    for name, expected in required_literals.items())
                and record.get("deadline_observation_semantics") ==
                    "TARGET_ACQUISITION_TRANSITION"
                and isinstance(endpoints, list) and len(endpoints) == 2
                and all(isinstance(value, list) for value in endpoints)
                and all(isinstance(value, list) for value in observed_endpoints)
                and all(set(machine) == set(observed) for machine, observed in zip(
                    endpoints, observed_endpoints))
                and isinstance(phase, dict)
                and phase.get("left_phase") == endpoints[0]
                and phase.get("right_phase") == endpoints[1]
                and phase.get("current_phase") == endpoints[1]
                and phase.get("left_phase") in (
                    phase.get("previous_phase"),
                    sorted(set(phase.get("previous_phase", [])) | set(endpoints[1])))
                and isinstance(changed, list) and bool(changed)
                and changed == sorted(set(endpoints[0]) ^ set(endpoints[1])))
    if classifier_id == "v1-stable-frequency-intact-context-v1":
        signature = record.get("event_signature")
        frequency = record.get("support_derived_frequency")
        required_literals = TEMPORAL_V2_OBSERVER_RUBRICS[classifier_id][
            "required_literals"]
        return (all(literal.get(name) == expected
                    for name, expected in required_literals.items())
                and record.get("deadline_observation_semantics") ==
                    "LEGAL_PRESENTATION_TRANSITION"
                and record.get("verification_closure_semantics") ==
                    "RAW_CURRENT_BRACKETED_UNRESOLVED_VERIFICATION_BOUNDARY"
                and record.get("branch") == "intact_mask"
                and isinstance(frequency, str)
                and re.fullmatch(r"[0-9]{2}\.[0-9]{3}", frequency) is not None
                and literal.get("observed_frequency") == frequency
                and isinstance(signature, dict)
                and signature.get("current_primary_frequency") == frequency)
    if classifier_id == "v1-secondary-closed-context-v3":
        required_literals = TEMPORAL_V2_OBSERVER_RUBRICS[classifier_id][
            "required_literals"]
        return (all(literal.get(name) == expected
                    for name, expected in required_literals.items())
                and record.get("deadline_observation_semantics") ==
                    "LEGAL_PRESENTATION_TRANSITION"
                and record.get("verification_closure_semantics") ==
                    "RAW_CURRENT_BRACKETED_UNRESOLVED_VERIFICATION_BOUNDARY"
                and record.get("auxiliary_closure_context_ns") == 80_000_000
                and literal.get("observed_cards") ==
                    record.get("support_derived_secondary")
                and record.get("support_derived_secondary") == record.get("resolved_value"))
    if classifier_id == "v1-secondary-text-optical-bridge-v1":
        required_literals = TEMPORAL_V2_OBSERVER_RUBRICS[classifier_id][
            "required_literals"]
        support = record.get("support_derived_secondary")
        slot = record.get("deficient_slot")
        raw_ocr = record.get("raw_frequency_only_ocr")
        return (all(literal.get(name) == expected
                    for name, expected in required_literals.items())
                and record.get("deadline_observation_semantics") ==
                    "LEGAL_PRESENTATION_TRANSITION"
                and support == record.get("resolved_value")
                and isinstance(support, list)
                and literal.get("support_cards") == support
                and literal.get("center_cards") == support
                and type(slot) is int and 0 <= slot < len(support)
                and isinstance(raw_ocr, dict)
                and raw_ocr.get("normalized_frequency") == support[slot].get("frequency"))
    if classifier_id == "v1-main-bar-adjacent-redraw-v2":
        endpoints = record.get("endpoint_values")
        return (isinstance(endpoints, list) and len(endpoints) == 2
                and all(type(value) is int and 0 <= value <= 6 for value in endpoints)
                and endpoints == [literal["left_endpoint_bar_count"],
                                  literal["right_endpoint_bar_count"]])
    signature = record.get("event_signature")
    if not isinstance(signature, dict):
        return False
    if classifier_id == "v1-muted-badge-rising-fill-v2":
        return (signature.get("previous_muted_badge") is False
                and signature.get("target_muted_badge") is True)
    if classifier_id == "v1-unmute-stable-frequency-sweep-v2":
        return (literal["left_endpoint_frequency"] ==
                signature.get("stable_primary_frequency") ==
                literal["right_endpoint_frequency"])
    return False


def _validate_temporal_v2_capture(capture: Any, window: Any, capture_path: Path,
                                  window_path: Path, references: dict[str, Any],
                                  bench_source_sha256: str) -> None:
    _require(isinstance(capture, dict) and capture.get("schema_version") == 1
             and capture.get("kind") == "blind_visible_reader_qualification_capture"
             and capture.get("capture_mode") == "--qualification-capture",
             "temporal qualification capture identity is invalid")
    source_git_sha = capture.get("source_git_sha")
    _require(isinstance(source_git_sha, str) and _GIT_SHA.fullmatch(source_git_sha) is not None,
             "temporal qualification capture source identity is invalid")
    _require(capture.get("bench_source_sha256") ==
             _digest(bench_source_sha256, "running bench.sh"),
             "temporal qualification capture bench implementation differs")

    collection = capture.get("collection")
    _require(isinstance(collection, dict)
             and collection.get("result") == "PASS"
             and collection.get("camera_result") == "CAPTURED"
             and collection.get("window_result") == window_path.name
             and collection.get("window_result_sha256") ==
                 references["window_result"].get("sha256")
             and _sha256(window_path) == collection.get("window_result_sha256"),
             "temporal qualification capture window binding differs")
    _require(capture_path.parent == window_path.parent,
             "temporal qualification capture files are not retained together")

    _require(isinstance(window, dict) and window.get("schema_version") == 5
             and window.get("suite") == "replay"
             and window.get("git_worktree_clean") is True
             and window.get("result") == "PASS"
             and isinstance(window.get("camera"), dict)
             and window["camera"].get("result") == "CAPTURED"
             and window.get("git_sha") == source_git_sha,
             "temporal qualification capture window is not a passing camera capture")
    runtime_identity = window.get("runtime_identity")
    runtime_qualification = window.get("runtime_qualification")
    runtime_git_sha = (runtime_identity.get("git_sha")
                       if isinstance(runtime_identity, dict) else None)
    _require(isinstance(runtime_git_sha, str) and 7 <= len(runtime_git_sha) <= 40
             and source_git_sha.startswith(runtime_git_sha)
             and isinstance(runtime_qualification, dict)
             and runtime_qualification.get("status") == "qualified"
             and runtime_qualification.get("git_match") is True,
             "temporal qualification capture runtime identity differs from its source")

    _require(capture.get("pixel_analysis") == {
        "status": "WITHHELD_BY_CAPTURE_MODE",
        "executed": [],
        "disabled": ["counter_check", "encounter_check"],
        "analyzer_outputs_present": False,
    } and capture.get("visible_product_eligible") is False,
             "temporal qualification capture did not withhold pixel analysis")


_SOURCE_FRAMEHASH_CACHE: dict[tuple[str, str], tuple[str, ...]] = {}
_TEMPORAL_V2_INSET_LOGICAL = {
    "v1-arrow-phase-edge-v5": (990, 190, 1165, 400),
    "v1-arrow-target-acquisition-v1": (990, 190, 1165, 400),
    "v1-stable-frequency-intact-context-v1": (425, 225, 845, 390),
    "v1-secondary-closed-context-v3": (385, 360, 880, 460),
    "v1-secondary-text-optical-bridge-v1": (385, 360, 880, 460),
    "v1-main-bar-adjacent-redraw-v2": (860, 185, 980, 440),
    "v1-muted-badge-rising-fill-v2": (480, 155, 710, 285),
    "v1-unmute-stable-frequency-sweep-v2": (425, 225, 845, 390),
}


def _temporal_v2_inset_box(classifier_id: str, registration: dict[str, Any],
                           width: int, height: int) -> list[int]:
    bounds = registration.get("landmark_bounds") if isinstance(registration, dict) else None
    _require(isinstance(bounds, list) and len(bounds) == 4
             and all(type(value) is int for value in bounds),
             f"temporal capture registration is malformed for {classifier_id}")
    x1, y1, x2, y2 = bounds
    scale = ((x2 - x1 + 1) / 220 * width / 1280,
             (y2 - y1 + 1) / 79 * height / 720)
    anchor = (x1 * width / 960, y1 * height / 540)
    origin = (376 * 4 / 3, 192 * 4 / 3)
    logical = _TEMPORAL_V2_INSET_LOGICAL[classifier_id]
    values = [round(anchor[index % 2] + (value - origin[index % 2]) * scale[index % 2])
              for index, value in enumerate(logical)]
    left, top, right, bottom = values
    result = [max(0, left), max(0, top), min(width, right), min(height, bottom)]
    _require(result[0] < result[2] and result[1] < result[3],
             f"temporal observer inset leaves retained source for {classifier_id}")
    return result


def _ffmpeg_framehashes(path: Path, video_filter: str) -> tuple[str, ...]:
    ffmpeg = shutil.which("ffmpeg")
    _require(ffmpeg is not None, "ffmpeg is required to verify temporal observer pixels")
    try:
        completed = subprocess.run(
            [ffmpeg, "-nostdin", "-hide_banner", "-loglevel", "error", "-i", str(path),
             "-an", "-vf", video_filter, "-fps_mode", "passthrough", "-f", "framehash",
             "-hash", "sha256", "pipe:1"],
            check=True, capture_output=True, text=True)
    except (OSError, subprocess.CalledProcessError) as exc:
        raise QualificationError(f"temporal media could not be decoded: {path.name}") from exc
    hashes = []
    for raw in completed.stdout.splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        fields = [field.strip() for field in line.split(",")]
        _require(len(fields) >= 6 and re.fullmatch(r"[0-9a-fA-F]{64}", fields[-1]) is not None,
                 f"temporal decoded-frame digest is malformed: {path.name}")
        hashes.append(fields[-1].lower())
    _require(bool(hashes), f"temporal media contains no decoded frames: {path.name}")
    return tuple(hashes)


def _ffprobe_lossless_clip(path: Path) -> dict[str, Any]:
    ffprobe = shutil.which("ffprobe")
    _require(ffprobe is not None, "ffprobe is required to verify temporal observer pixels")
    try:
        completed = subprocess.run(
            [ffprobe, "-v", "error", "-select_streams", "v:0", "-show_entries",
             "stream=codec_name,pix_fmt,width,height,nb_frames,r_frame_rate", "-of", "json",
             str(path)], check=True, capture_output=True, text=True)
        payload = json.loads(completed.stdout)
        streams = payload.get("streams")
        _require(isinstance(streams, list) and len(streams) == 1
                 and isinstance(streams[0], dict),
                 f"temporal observer clip probe is malformed: {path.name}")
        return streams[0]
    except (OSError, subprocess.CalledProcessError, json.JSONDecodeError) as exc:
        raise QualificationError(f"temporal observer clip probe failed: {path.name}") from exc


def _validate_temporal_v2_media(classifier_id: str, source_paths: dict[str, Path],
                                sources: dict[str, Any], window: dict[str, Any],
                                analysis_selection: dict[str, Any],
                                manifest_items: list[dict[str, Any]]) -> dict[int, dict[str, int]]:
    """Re-establish the retained capture and exact lossless observer pixels."""
    try:
        from camera_artifacts import load_capture_manifest, verify_capture_files
        from camera_timing import load_frame_sidecar

        capture_manifest_path = source_paths["capture_manifest"]
        camera_root = capture_manifest_path.parent
        manifest = load_capture_manifest(capture_manifest_path)
        verify_capture_files(camera_root, manifest)
        _require(manifest.get("capture_id") == window.get("camera", {}).get("capture_id"),
                 f"retained temporal capture identity differs for {classifier_id}")
        artifacts = manifest["identity"]["artifacts"]
        owned = {
            "qualification_video": "video",
            "frame_timing": "frame_timing",
            "video_timing_verification": "video_timing_verification",
        }
        for source_name, artifact_name in owned.items():
            entry = artifacts.get(artifact_name)
            path = source_paths[source_name]
            _require(isinstance(entry, dict)
                     and path.parent.resolve() == camera_root.resolve()
                     and path.name == entry.get("path")
                     and _sha256(path) == entry.get("sha256"),
                     f"retained temporal capture artifact differs: {source_name}")
        timing = sources["video_timing_verification"]
        window_timing = window.get("camera", {}).get("video_timing_verification_result")
        _require(timing == window_timing and timing.get("status") == "verified",
                 f"retained temporal timing differs for {classifier_id}")
        for name in ("timestamp_error_count", "missing_encoded_frame_count",
                     "extra_encoded_frame_count", "duration_mismatch_count"):
            _require(type(timing.get(name)) is int and timing[name] == 0,
                     f"retained temporal timing is not exact: {name}")
        records = load_frame_sidecar(source_paths["frame_timing"])
        _require(all(record.get("phase") == "recording" for record in records),
                 f"retained temporal sidecar mixes camera phases for {classifier_id}")
        rows = [record for record in records if record.get("status") == "written"]
        _require(bool(rows)
                 and timing.get("written_frame_count") == len(rows)
                 and timing.get("encoded_frame_count") == len(rows)
                 and timing.get("source_frame_count") == len(records),
                 f"retained temporal sidecar denominator differs for {classifier_id}")
        row_by_index = {
            index: {"source_frame_seq": row["frame_seq"], "capture_ns": row["host_capture_ns"]}
            for index, row in enumerate(rows)
        }
        samples = analysis_selection.get("samples")
        _require(isinstance(samples, list),
                 f"temporal analysis selection is malformed for {classifier_id}")
        for sample in samples:
            index = sample.get("video_frame_index") if isinstance(sample, dict) else None
            _require(type(index) is int and row_by_index.get(index) == {
                         "source_frame_seq": sample.get("source_frame_seq"),
                         "capture_ns": sample.get("capture_ns")},
                     f"temporal analysis selection differs from sidecar at frame {index}")

        probe = manifest.get("capture", {}).get("video_probe")
        width = probe.get("width") if isinstance(probe, dict) else None
        height = probe.get("height") if isinstance(probe, dict) else None
        _require(type(width) is int and type(height) is int and width > 0 and height > 0,
                 f"retained temporal video dimensions are invalid for {classifier_id}")
        preflight_entry = artifacts.get("preflight")
        _require(isinstance(preflight_entry, dict), "retained temporal preflight is missing")
        preflight_path = camera_root / str(preflight_entry.get("path") or "")
        preflight = json.loads(preflight_path.read_text(encoding="utf-8"))
        registration = preflight.get("registration") if isinstance(preflight, dict) else None
        _require(isinstance(preflight, dict) and preflight.get("result") == "PASS"
                 and isinstance(registration, dict)
                 and registration.get("result") == "PASS",
                 f"retained temporal registration did not pass for {classifier_id}")
        inset_box = _temporal_v2_inset_box(classifier_id, registration, width, height)

        video = source_paths["qualification_video"]
        # Each classifier has a hardlink view of the same retained capture.  Its
        # independently verified manifest digest is the stable cache identity;
        # path identity would decode a long 200 fps capture three times.
        cache_key = (artifacts["video"]["sha256"], f"{width}x{height}:rgb24")
        source_hashes = _SOURCE_FRAMEHASH_CACHE.get(cache_key)
        if source_hashes is None:
            source_hashes = _ffmpeg_framehashes(video, "format=rgb24")
            _SOURCE_FRAMEHASH_CACHE[cache_key] = source_hashes
        _require(len(source_hashes) == len(rows),
                 f"retained temporal video frame denominator differs for {classifier_id}")
        for item in manifest_items:
            opaque_id = item.get("opaque_id")
            clip = _evidence_file(
                source_paths["observer_manifest"].parent,
                {"path": item.get("clip"), "sha256": item.get("sha256")},
                f"temporal observer clip {opaque_id}")
            _require(type(item.get("size_bytes")) is int
                     and clip.stat().st_size == item["size_bytes"],
                     f"temporal observer clip size differs: {opaque_id}")
            indices = item.get("clip_source_video_indices")
            target = item.get("target_run_video_indices")
            scope = (temporal_acquisition_observer_scope(item)
                     if classifier_id == "v1-arrow-target-acquisition-v1" else None)
            expected_indices = (temporal_observer_clip_source_indices(
                                    target, row_by_index,
                                    [index for values in scope.values() for index in values]
                                    if scope is not None else None)
                                if isinstance(target, list) and target else [])
            _require(isinstance(indices, list) and bool(indices)
                     and all(type(index) is int and 0 <= index < len(source_hashes)
                             for index in indices)
                     and indices == expected_indices,
                     f"temporal observer clip source range is invalid: {opaque_id}")
            stream = _ffprobe_lossless_clip(clip)
            _require(stream.get("codec_name") == "png" and stream.get("pix_fmt") == "rgb24"
                     and stream.get("width") == width + 440 and stream.get("height") == height
                     and stream.get("r_frame_rate") == "25/1"
                     and int(stream.get("nb_frames", -1)) == len(indices),
                     f"temporal observer clip is not the required lossless view: {opaque_id}")
            left_hashes = _ffmpeg_framehashes(
                clip, f"crop={width}:{height}:0:0,format=rgb24")
            _require(left_hashes == tuple(source_hashes[index] for index in indices),
                     f"temporal observer clip pixels differ from retained video: {opaque_id}")
            left, top, right, bottom = inset_box
            expected_inset = _ffmpeg_framehashes(
                clip,
                f"crop={width}:{height}:0:0,crop={right-left}:{bottom-top}:{left}:{top},"
                "scale=440:680:force_original_aspect_ratio=decrease:flags=neighbor,"
                "pad=440:720:(ow-iw)/2:(oh-ih)/2:black,format=rgb24")
            retained_inset = _ffmpeg_framehashes(
                clip, f"crop=440:{height}:{width}:0,format=rgb24")
            _require(expected_inset == retained_inset
                     and item.get("inset_source_box") == inset_box,
                     f"temporal observer inset differs from its source pixels: {opaque_id}")
        return row_by_index
    except QualificationError:
        raise
    except (KeyError, OSError, TypeError, ValueError, json.JSONDecodeError) as exc:
        raise QualificationError(
            f"retained temporal capture could not be verified for {classifier_id}: {exc}") from exc


def _finite_number(value: Any) -> bool:
    return (type(value) in (int, float) and not isinstance(value, bool)
            and math.isfinite(value))


def _require_point(point: Any, video_index: int, name: str,
                   source_rows: dict[int, dict[str, int]] | None = None) -> None:
    _require(isinstance(point, dict)
             and point.get("video_frame_index") == video_index
             and type(point.get("source_frame_seq")) is int
             and type(point.get("capture_ns")) is int,
             f"temporal classifier {name} point is malformed")
    if source_rows is not None:
        _require(source_rows.get(video_index) == {
                     "source_frame_seq": point["source_frame_seq"],
                     "capture_ns": point["capture_ns"]},
                 f"temporal classifier {name} point differs from retained sidecar")


def _temporal_v2_context_binding(classifier_id: str, spec_document: dict[str, Any],
                                 window: dict[str, Any], references: dict[str, Any],
                                 implementation: dict[str, str],
                                 reader: dict[str, Any]) -> dict[str, Any]:
    identity = spec_document.get("identity")
    constants = spec_document.get("constants")
    camera = window.get("camera")
    timing = camera.get("video_timing_verification_result") if isinstance(camera, dict) else None
    _require(isinstance(identity, dict) and isinstance(constants, dict)
             and isinstance(camera, dict) and isinstance(timing, dict),
             f"temporal classifier context sources are malformed for {classifier_id}")
    capture_id = _digest(camera.get("capture_id"), f"{classifier_id} capture")
    maximum_interval = timing.get("maximum_source_interval_ns")
    common_valid = (timing.get("status") == "verified"
                    and type(maximum_interval) is int
                    and 0 < maximum_interval <= 1_000_000_000
                    and identity.get("reader_method_version") == reader.get("method_version")
                    and identity.get("reader_sha256") == implementation.get("encounter_reader.py"))
    if classifier_id == "v1-arrow-phase-edge-v5":
        _require(common_valid and set(identity) == {"reader_method_version", "reader_sha256"},
                 f"temporal classifier context identity differs for {classifier_id}")
        return {
            "capture_id": capture_id,
            "selection_manifest_sha256": references["analysis_selection"]["sha256"],
            "verified_maximum_source_interval_ns": maximum_interval,
            "reader_method_version": reader["method_version"],
            "reader_sha256": implementation["encounter_reader.py"],
        }
    if classifier_id == "v1-arrow-target-acquisition-v1":
        configured_maximum = constants.get("maximum_recording_interval_ns")
        support_maximum = constants.get("maximum_support_interval_ns")
        _require(common_valid
                 and set(identity) == {"reader_method_version", "reader_sha256"}
                 and type(configured_maximum) is int
                 and type(support_maximum) is int
                 and 0 < support_maximum <= configured_maximum
                 and 0 < maximum_interval <= configured_maximum,
                 f"temporal classifier context identity differs for {classifier_id}")
        return {
            "capture_id": capture_id,
            "selection_manifest_sha256": references["analysis_selection"]["sha256"],
            "verified_maximum_source_interval_ns": maximum_interval,
            "reader_method_version": reader["method_version"],
            "reader_sha256": implementation["encounter_reader.py"],
        }
    if classifier_id == "v1-secondary-text-optical-bridge-v1":
        configured_maximum = constants.get("maximum_recording_interval_ns")
        support_maximum = constants.get("maximum_support_chain_interval_ns")
        _require(common_valid
                 and set(identity) == {
                     "reader_method_version", "reader_sha256",
                     "secondary_probe_method_version", "secondary_probe_sha256"}
                 and type(configured_maximum) is int
                 and type(support_maximum) is int
                 and 0 < support_maximum <= configured_maximum
                 and 0 < maximum_interval <= configured_maximum
                 and type(identity.get("secondary_probe_method_version")) is int
                 and identity.get("secondary_probe_sha256") ==
                     implementation.get("encounter_secondary_probe.py"),
                 f"temporal classifier context identity differs for {classifier_id}")
        return {
            "capture_id": capture_id,
            "selection_manifest_sha256": references["analysis_selection"]["sha256"],
            "verified_maximum_source_interval_ns": maximum_interval,
            "reader_method_version": reader["method_version"],
            "reader_sha256": implementation["encounter_reader.py"],
            "secondary_probe_method_version": identity["secondary_probe_method_version"],
            "secondary_probe_sha256": implementation["encounter_secondary_probe.py"],
        }
    if classifier_id == "v1-secondary-closed-context-v3":
        configured_maximum = constants.get("maximum_recording_interval_ns")
        support_maximum = constants.get("maximum_support_chain_interval_ns")
        _require(common_valid
                 and set(identity) == {"reader_method_version", "reader_sha256"}
                 and type(configured_maximum) is int
                 and type(support_maximum) is int
                 and 0 < support_maximum <= configured_maximum
                 and 0 < maximum_interval <= configured_maximum,
                 f"temporal classifier context identity differs for {classifier_id}")
        return {
            "capture_id": capture_id,
            "selection_manifest_sha256": references["analysis_selection"]["sha256"],
            "verified_maximum_source_interval_ns": maximum_interval,
            "reader_method_version": reader["method_version"],
            "reader_sha256": implementation["encounter_reader.py"],
        }
    configured_maximum = constants.get("maximum_recording_interval_ns")
    support_maximum = constants.get("maximum_support_interval_ns")
    _require(common_valid
             and set(identity) == {
                 "reader_method_version", "reader_sha256",
                 "redraw_probe_method_version", "redraw_probe_sha256"}
             and type(configured_maximum) is int
             and type(support_maximum) is int
             and 0 < support_maximum <= configured_maximum
             and 0 < maximum_interval <= configured_maximum
             and type(identity.get("redraw_probe_method_version")) is int
             and identity.get("redraw_probe_sha256") ==
                 implementation.get("encounter_redraw_probe.py"),
             f"temporal classifier context identity differs for {classifier_id}")
    return {
        "capture_id": capture_id,
        "selection_manifest_sha256": references["analysis_selection"]["sha256"],
        "verified_maximum_source_interval_ns": maximum_interval,
        "reader_method_version": reader["method_version"],
        "reader_sha256": implementation["encounter_reader.py"],
        "redraw_probe_method_version": identity["redraw_probe_method_version"],
        "redraw_probe_sha256": implementation["encounter_redraw_probe.py"],
    }


def _validate_temporal_support_chain(first: int, last: int,
                                       source_rows: dict[int, dict[str, int]],
                                       maximum_gap_ns: int) -> None:
    """Bind a local transition's timing to its actual authenticated source rows."""
    chain = [source_rows.get(index) for index in range(first, last + 1)]
    _require(bool(chain) and all(isinstance(row, dict) for row in chain),
             "temporal support chain is missing source frames")
    # The authenticated map is keyed by video index; requiring every key in
    # this consecutive range also rules out a missing or skipped video frame.
    _require(all(type(row.get("source_frame_seq")) is int
                 and type(row.get("capture_ns")) is int for row in chain),
             "temporal support chain has malformed source identity")
    _require(all(right["source_frame_seq"] == left["source_frame_seq"] + 1
                 and 0 < right["capture_ns"] - left["capture_ns"] <= maximum_gap_ns
                 for left, right in zip(chain, chain[1:])),
             "temporal support chain exceeds its local source gap bound")


def _validate_temporal_v2_common_record(classifier_id: str, spec_sha256: str,
                                        record: dict[str, Any], target_indices: list[int],
                                        context: dict[str, Any], required_keys: set[str],
                                        source_rows: dict[int, dict[str, int]]) -> None:
    _require(set(record) == required_keys
             and isinstance(record.get("event_id"), str) and bool(record["event_id"])
             and record.get("classifier_id") == classifier_id
             and record.get("classifier_spec_sha256") == spec_sha256
             and record.get("status") == "QUALIFIED_CAPTURE_TRANSITION"
             and record.get("raw_affected_fields") ==
                 TEMPORAL_V2_OBSERVER_RUBRICS[classifier_id]["raw_affected_fields"]
             and record.get("video_frame_indices") == target_indices
             and isinstance(record.get("basis"), str) and bool(record["basis"]),
             "temporal admitted classifier record shape differs")
    for name, expected in context.items():
        _require(record.get(name) == expected,
                 f"temporal admitted classifier record context differs: {name}")
    _require_point(record.get("first"), target_indices[0], "first", source_rows)
    _require_point(record.get("last"), target_indices[-1], "last", source_rows)


def _validate_temporal_v2_bar_record(record: dict[str, Any], target_indices: list[int],
                                     context: dict[str, Any], spec_document: dict[str, Any],
                                     source_rows: dict[int, dict[str, int]]) -> None:
    constants = spec_document["constants"]
    profile = spec_document.get("profile")
    endpoints = record.get("endpoint_values")
    signature = record.get("main_bar_expectation_signature")
    _require(isinstance(endpoints, list) and len(endpoints) == 2
             and all(type(value) is int and 0 <= value <= 6 for value in endpoints)
             and abs(endpoints[1] - endpoints[0]) == 1
             and signature == {"previous_count": endpoints[0], "current_count": endpoints[1]}
             and record.get("changed_bar_index") == min(endpoints),
             "temporal main-bar endpoint record is inconsistent")
    first, last = target_indices[0], target_indices[-1]
    for name, index in (("left_support", first - 2), ("left_endpoint", first - 1),
                        ("right_endpoint", last + 1), ("right_support", last + 2)):
        _require_point(record.get(name), index, name, source_rows)
    local_gap = min(context["verified_maximum_source_interval_ns"],
                    constants["maximum_support_interval_ns"])
    _validate_temporal_support_chain(first - 2, last + 2, source_rows, local_gap)
    maximum_span = constants["authored_display_update_ns"] + local_gap
    _require(source_rows[last + 1]["capture_ns"] - source_rows[first - 1]["capture_ns"]
             <= maximum_span, "temporal main-bar endpoints exceed their local span bound")
    _require(isinstance(profile, dict)
             and record.get("profile_schema") == {
                 "rows": profile.get("rows"), "columns": profile.get("columns"),
                 "sample": profile.get("sample")}
             and record.get("profile_boxes") == profile.get("bar_boxes_bottom_to_top")
             and record.get("maximum_endpoint_span_ns") == maximum_span,
             "temporal main-bar profile contract differs")
    separation = record.get("endpoint_separation_rms")
    projections = record.get("projections")
    residuals = record.get("normalized_residuals")
    _require(_finite_number(separation)
             and separation >= constants.get("endpoint_separation_rms_min")
             and isinstance(projections, list) and len(projections) == len(target_indices)
             and isinstance(residuals, list) and len(residuals) == len(target_indices)
             and all(_finite_number(value)
                     and constants["projection_min"] <= value <= constants["projection_max"]
                     for value in projections)
             and all(_finite_number(value)
                     and 0 <= value <= constants["normalized_residual_max"]
                     for value in residuals),
             "temporal main-bar interpolation metrics are invalid")
    path = [0.0, *projections, 1.0]
    backwards = [max(0.0, left - right) for left, right in zip(path, path[1:])]
    _require(record.get("maximum_backward_step") == max(backwards, default=0.0)
             and record["maximum_backward_step"] <= constants["maximum_backward_step"]
             and record.get("total_backward_motion") == sum(backwards)
             and record["total_backward_motion"] <= constants["maximum_total_backward_motion"],
             "temporal main-bar projection motion is inconsistent")
    medians = record.get("boundary_medians")
    _require(isinstance(medians, list) and len(medians) == len(target_indices) + 2
             and all(_finite_number(value) for value in medians),
             "temporal main-bar boundary medians are invalid")
    direction = 1.0 if medians[-1] >= medians[0] else -1.0
    median_backwards = [max(0.0, -direction * (right - left))
                        for left, right in zip(medians, medians[1:])]
    _require(record.get("maximum_boundary_median_backward_step") ==
             max(median_backwards, default=0.0)
             and record["maximum_boundary_median_backward_step"] <=
                 constants["boundary_median_backward_tolerance"],
             "temporal main-bar boundary motion is inconsistent")
    diameters = record.get("unchanged_cell_profile_diameter_rms")
    expected_cells = {str(index) for index in range(6)
                      if index != record["changed_bar_index"]}
    _require(isinstance(diameters, dict) and set(diameters) == expected_cells
             and all(_finite_number(value) and 0 <= value <=
                     constants["unchanged_cell_profile_diameter_rms_max"]
                     for value in diameters.values()),
             "temporal main-bar unchanged-cell metrics are invalid")


def _validate_temporal_v2_arrow_record(record: dict[str, Any], target_indices: list[int],
                                       context: dict[str, Any], spec_document: dict[str, Any],
                                       source_rows: dict[int, dict[str, int]]) -> None:
    constants = spec_document["constants"]
    directions = {"front", "side", "rear"}
    endpoints = record.get("endpoint_values")
    _require(isinstance(endpoints, list) and len(endpoints) == 2
             and all(isinstance(value, list)
                     and value == sorted(value)
                     and len(value) == len(set(value))
                     and set(value) <= directions for value in endpoints),
             "temporal arrow endpoint record is malformed")
    endpoint_sets = {tuple(value) for value in endpoints}
    changed = set(endpoints[0]) ^ set(endpoints[1])
    signature = record.get("arrow_expectation_signature")
    expected_phases = [list(value) for value in sorted(endpoint_sets)]
    _require(len(endpoint_sets) == 2 and len(changed) == 1
             and record.get("changed_direction") == next(iter(changed))
             and signature == {"allowed_arrow_sets": expected_phases,
                               "joint_arrow_phases": expected_phases},
             "temporal arrow phase association is inconsistent")
    first, last = target_indices[0], target_indices[-1]
    for name, index in (("left_support", first - 3), ("left_endpoint", first - 2),
                        ("right_endpoint", last + 2), ("right_support", last + 3)):
        _require_point(record.get(name), index, name, source_rows)
    maximum_gap = context["verified_maximum_source_interval_ns"]
    _validate_temporal_support_chain(first - 3, last + 3, source_rows, maximum_gap)
    maximum_span = constants["authored_blink_phase_ns"] + maximum_gap
    _require(source_rows[last + 2]["capture_ns"] - source_rows[first - 2]["capture_ns"]
             <= maximum_span, "temporal arrow endpoints exceed their span bound")
    _require(record.get("maximum_endpoint_span_ns") == maximum_span
             and record.get("profile_schema") == {
                 "rows": 4, "columns": 4, "cells": 16,
                 "sample": "max-channel cell median"},
             "temporal arrow profile contract differs")
    bounds = record.get("profile_reference_bounds")
    _require(isinstance(bounds, dict) and set(bounds) == directions
             and all(isinstance(value, list) and len(value) == 4
                     and all(type(coordinate) is int for coordinate in value)
                     for value in bounds.values()),
             "temporal arrow profile bounds are malformed")
    separation = record.get("endpoint_separation_rms")
    projections = record.get("projections")
    residuals = record.get("normalized_residuals")
    _require(_finite_number(separation)
             and separation >= constants["endpoint_separation_rms_min"]
             and record.get("profile_frame_indices") == list(range(first - 2, last + 3))
             and isinstance(projections, list) and len(projections) == len(target_indices) + 4
             and isinstance(residuals, list) and len(residuals) == len(target_indices) + 4
             and all(_finite_number(value)
                     and constants["projection_min"] <= value <= constants["projection_max"]
                     for value in projections)
             and all(_finite_number(value)
                     and 0 <= value <= constants["normalized_residual_max"]
                     for value in residuals),
             "temporal arrow interpolation metrics are invalid")
    path = [0.0, *projections, 1.0]
    backwards = [max(0.0, left - right) for left, right in zip(path, path[1:])]
    _require(record.get("maximum_backward_step") == max(backwards, default=0.0)
             and record["maximum_backward_step"] <= constants["maximum_backward_step"]
             and record.get("total_backward_motion") == sum(backwards)
             and record["total_backward_motion"] <= constants["maximum_total_backward_motion"],
             "temporal arrow projection motion is inconsistent")
    diameters = record.get("extra_direction_profile_diameter_rms")
    expected_extra = directions - {record["changed_direction"]}
    _require(isinstance(diameters, dict) and set(diameters) == expected_extra
             and all(_finite_number(value) and 0 <= value <=
                     constants["extra_direction_profile_diameter_rms_max"]
                     for value in diameters.values()),
             "temporal arrow extra-direction metrics are invalid")


def _validate_temporal_v2_arrow_acquisition_record(
        record: dict[str, Any], target_indices: list[int], context: dict[str, Any],
        spec_document: dict[str, Any], source_rows: dict[int, dict[str, int]]) -> None:
    constants = spec_document["constants"]
    profile = spec_document.get("profile")
    directions = {"front", "side", "rear"}

    def phase(value: Any) -> tuple[str, ...] | None:
        if (not isinstance(value, list) or value != sorted(value)
                or len(value) != len(set(value)) or not set(value) <= directions):
            return None
        return tuple(value)

    endpoints = record.get("endpoint_values")
    signature = record.get("arrow_expectation_signature")
    endpoint_basis = record.get("endpoint_phase_basis")
    _require(isinstance(endpoints, list) and len(endpoints) == 2
             and all(phase(value) is not None for value in endpoints)
             and isinstance(signature, dict)
             and set(signature) == {"previous_arrow_sets", "current_arrow_sets"}
             and all(isinstance(values, list) and bool(values)
                     and all(phase(value) is not None for value in values)
                     and len({tuple(value) for value in values}) == len(values)
                     for values in signature.values())
             and isinstance(endpoint_basis, dict)
             and set(endpoint_basis) == {
                 "previous_phase", "current_phase", "left_phase", "right_phase"}
             and all(phase(value) is not None for value in endpoint_basis.values()),
             "temporal arrow acquisition endpoint record is malformed")
    left, right = map(tuple, endpoints)
    previous = {tuple(value) for value in signature["previous_arrow_sets"]}
    current = {tuple(value) for value in signature["current_arrow_sets"]}
    prior = tuple(endpoint_basis["previous_phase"])
    _require(previous != current and prior in previous and prior != right
             and right in current and left not in current
             and tuple(endpoint_basis["current_phase"]) == right
             and tuple(endpoint_basis["left_phase"]) == left
             and tuple(endpoint_basis["right_phase"]) == right
             and left in {prior, tuple(sorted(set(prior) | set(right)))},
             "temporal arrow acquisition phase association is inconsistent")
    changed = sorted(set(left) ^ set(right))
    _require(bool(changed) and record.get("changed_directions") == changed,
             "temporal arrow acquisition changed directions are inconsistent")
    claimed_proof = record.get("claimed_frame_acquisition_proof")
    _require(isinstance(claimed_proof, list)
             and len(claimed_proof) == len(target_indices),
             "temporal arrow acquisition claimed-frame proof is incomplete")
    for proof, video_index in zip(claimed_proof, target_indices):
        states = proof.get("changed_direction_states") if isinstance(proof, dict) else None
        noncurrent = proof.get("noncurrent_changed_directions") if isinstance(proof, dict) else None
        _require(isinstance(proof, dict)
                 and set(proof) == {
                     "video_frame_index", "changed_direction_states",
                     "noncurrent_changed_directions"}
                 and proof.get("video_frame_index") == video_index
                 and isinstance(states, dict) and set(states) == set(changed)
                 and all(state in {"filled", "unlit", "partial", "faint"}
                         for state in states.values())
                 and noncurrent == sorted(
                     direction for direction, state in states.items()
                     if state != ("filled" if direction in right else "unlit"))
                 and bool(noncurrent),
                 "temporal arrow acquisition claimed-frame proof is malformed")

    full_indices = record.get("full_transition_indices")
    _require(isinstance(full_indices, list) and bool(full_indices)
             and all(type(value) is int for value in full_indices)
             and all(next_value == value + 1
                     for value, next_value in zip(full_indices, full_indices[1:]))
             and all(value in full_indices for value in target_indices),
             "temporal arrow acquisition full transition is invalid")
    supports = record.get("left_support"), record.get("right_support")
    _require(all(isinstance(value, list) and len(value) == 2 for value in supports),
             "temporal arrow acquisition support record is malformed")
    for point, index in zip([*supports[0], *supports[1]],
                            (full_indices[0] - 2, full_indices[0] - 1,
                             full_indices[-1] + 1, full_indices[-1] + 2)):
        _require_point(point, index, "arrow acquisition support", source_rows)
    local_gap = min(context["verified_maximum_source_interval_ns"],
                    constants["maximum_support_interval_ns"])
    _validate_temporal_support_chain(
        full_indices[0] - 2, full_indices[-1] + 2, source_rows, local_gap)
    maximum_span = constants["authored_display_update_ns"] + local_gap
    _require(source_rows[full_indices[-1] + 1]["capture_ns"]
             - source_rows[full_indices[0] - 1]["capture_ns"] <= maximum_span
             and record.get("maximum_endpoint_span_ns") == maximum_span
             and record.get("support_search_frames_each_side") ==
                 constants["support_search_frames_each_side"],
             "temporal arrow acquisition support span is inconsistent")

    _require(isinstance(profile, dict)
             and profile.get("rows") == 4 and profile.get("columns") == 4
             and profile.get("cells") == 16
             and profile.get("finite_range") == [0.0, 255.0]
             and profile.get("reference_bounds_must_match") is True
             and record.get("profile_schema") == {
                 "rows": 4, "columns": 4, "cells": 16,
                 "sample": "max-channel cell median"},
             "temporal arrow acquisition profile contract differs")
    bounds = record.get("profile_reference_bounds")
    _require(isinstance(bounds, dict) and set(bounds) == directions
             and all(isinstance(value, list) and len(value) == 4
                     and all(type(coordinate) is int for coordinate in value)
                     for value in bounds.values()),
             "temporal arrow acquisition profile bounds are malformed")

    metrics = record.get("direction_metrics")
    metric_keys = {
        "endpoint_separation_rms", "projections", "normalized_residuals",
        "maximum_backward_step", "total_backward_motion"}
    _require(isinstance(metrics, dict) and set(metrics) == set(changed),
             "temporal arrow acquisition direction metrics are incomplete")
    for direction, values in metrics.items():
        _require(isinstance(values, dict) and set(values) == metric_keys,
                 f"temporal arrow acquisition metrics are malformed for {direction}")
        separation = values["endpoint_separation_rms"]
        projections = values["projections"]
        residuals = values["normalized_residuals"]
        _require(_finite_number(separation)
                 and separation >= constants["endpoint_separation_rms_min"]
                 and isinstance(projections, list) and len(projections) == len(full_indices)
                 and isinstance(residuals, list) and len(residuals) == len(full_indices)
                 and all(_finite_number(value)
                         and constants["projection_min"] <= value <= constants["projection_max"]
                         for value in projections)
                 and all(_finite_number(value)
                         and 0 <= value <= constants["normalized_residual_max"]
                         for value in residuals),
                 f"temporal arrow acquisition optical path is invalid for {direction}")
        path = [0.0, *projections, 1.0]
        backwards = [max(0.0, before - after)
                     for before, after in zip(path, path[1:])]
        _require(values["maximum_backward_step"] == max(backwards, default=0.0)
                 and values["maximum_backward_step"] <= constants["maximum_backward_step"]
                 and values["total_backward_motion"] == sum(backwards)
                 and values["total_backward_motion"] <=
                     constants["maximum_total_backward_motion"],
                 f"temporal arrow acquisition motion is inconsistent for {direction}")
    unchanged = record.get("unchanged_direction_profile_diameter_rms")
    _require(isinstance(unchanged, dict) and set(unchanged) == directions - set(changed)
             and all(_finite_number(value) and 0 <= value <=
                     constants["unchanged_direction_profile_diameter_rms_max"]
                     for value in unchanged.values()),
             "temporal arrow acquisition unchanged-direction metrics are invalid")


_DIGIT_MASKS = {
    "0": "abcdef", "1": "bc", "2": "abdeg", "3": "abcdg", "4": "bcfg",
    "5": "acdfg", "6": "acdefg", "7": "abc", "8": "abcdefg", "9": "abcdfg",
}


def _validate_temporal_v2_frequency_context_record(
        record: dict[str, Any], target_indices: list[int], context: dict[str, Any],
        spec_document: dict[str, Any], source_rows: dict[int, dict[str, int]]) -> None:
    constants = spec_document["constants"]
    branches = spec_document.get("branches")
    profile = spec_document.get("profile")
    branch = record.get("branch")
    reasons = {"intact_mask": "inconsistent illuminated frequency segment levels"}
    _require(branch in reasons
             and branches == {
                 "intact_mask": {
                     "ambiguity_reason": reasons["intact_mask"],
                     "maximum_partial_expected_on_segments": 0,
                 },
             }
             and record.get("deadline_observation_semantics") ==
                 "LEGAL_PRESENTATION_TRANSITION"
             and spec_document.get("verification_closure_semantics") ==
                 "RAW_CURRENT_BRACKETED_UNRESOLVED_VERIFICATION_BOUNDARY"
             and record.get("verification_closure_semantics") ==
                 spec_document.get("verification_closure_semantics")
             and record.get("ambiguity_reason") == reasons[branch],
             "temporal frequency-context branch contract differs")
    _require(constants == {
                 "stable_support_frames_each_side": 2,
                 "reader_on_p10_min": 45.0, "reader_off_p90_max": 32.0,
                 "maximum_hole_ink_fraction": 0.1,
                 "maximum_recording_interval_ns": 1_000_000_000,
                 "maximum_support_interval_ns": 10_000_000,
                 "maximum_refusal_run_span_ns": 75_000_000,
                 "maximum_support_chain_span_ns": 300_000_000,
             },
             "temporal frequency-context constants differ")
    _require(profile == {
                 "decimal_box": [590, 349, 599, 357],
                 "digit_origins": [454, 520, 616, 688, 764],
                 "finite_range": [0.0, 255.0],
                 "hole_bounds_per_digit": [[28, 278, 40, 292], [28, 322, 40, 338]],
                 "sample": "max-channel segment p10, median, and p90",
                 "segments": ["a", "b", "c", "d", "e", "f", "g"],
             },
             "temporal frequency-context profile contract differs")

    frequency = record.get("support_derived_frequency")
    _require(isinstance(frequency, str)
             and re.fullmatch(r"[0-9]{2}\.[0-9]{3}", frequency) is not None,
             "temporal frequency-context support value is malformed")
    expected_masks = [_DIGIT_MASKS[digit] for digit in frequency.replace(".", "")]
    _require(record.get("support_derived_digit_masks") == expected_masks,
             "temporal frequency-context digit masks differ from its support value")
    signature = record.get("event_signature")
    _require(isinstance(signature, dict)
             and set(signature) == {"mode", "changed_fields", "current_primary_frequency"}
             and signature.get("mode") in {"BASELINE", "UNCHANGED", "CHANGED"}
             and isinstance(signature.get("changed_fields"), list)
             and len(signature["changed_fields"]) == len(set(signature["changed_fields"]))
             and set(signature["changed_fields"]) <= set(FIELDS)
             and signature.get("current_primary_frequency") == frequency,
             "temporal frequency-context event signature is malformed")

    full_indices = record.get("context_frame_indices")
    _require(isinstance(full_indices, list) and bool(full_indices)
             and all(type(value) is int for value in full_indices)
             and all(right == left + 1 for left, right in zip(full_indices, full_indices[1:]))
             and all(value in full_indices for value in target_indices),
             "temporal frequency-context closed run is invalid")
    observed_branches = record.get("context_observed_branches")
    _require(observed_branches == ["intact_mask"],
             "temporal frequency-context observed branches are malformed")
    supports = record.get("left_support"), record.get("right_support")
    _require(all(isinstance(value, list) and len(value) == 2 for value in supports),
             "temporal frequency-context support record is malformed")
    for point, index in zip([*supports[0], *supports[1]],
                            (full_indices[0] - 2, full_indices[0] - 1,
                             full_indices[-1] + 1, full_indices[-1] + 2)):
        _require_point(point, index, "frequency-context support", source_rows)
    local_gap = min(context["verified_maximum_source_interval_ns"],
                    constants["maximum_support_interval_ns"])
    _validate_temporal_support_chain(
        full_indices[0] - 2, full_indices[-1] + 2, source_rows, local_gap)
    _require(source_rows[target_indices[-1]]["capture_ns"]
             - source_rows[target_indices[0]]["capture_ns"] <=
                 constants["maximum_refusal_run_span_ns"]
             and source_rows[full_indices[-1] + 2]["capture_ns"]
             - source_rows[full_indices[0] - 2]["capture_ns"] <=
                 constants["maximum_support_chain_span_ns"]
             and record.get("maximum_refusal_run_span_ns") ==
                 constants["maximum_refusal_run_span_ns"]
             and record.get("maximum_support_chain_span_ns") ==
                 constants["maximum_support_chain_span_ns"],
             "temporal frequency-context span or threshold binding differs")



def _validate_temporal_v2_secondary_context_record(
        record: dict[str, Any], target_indices: list[int], context: dict[str, Any],
        spec_document: dict[str, Any], source_rows: dict[int, dict[str, int]]) -> None:
    constants = spec_document.get("constants")
    profile = spec_document.get("profile")
    _require(constants == {
                 "authored_display_update_ns": 50_000_000,
                 "maximum_interleaved_readable_frames": 2,
                 "maximum_recording_interval_ns": 1_000_000_000,
                 "maximum_support_chain_interval_ns": 10_000_000,
                 "maximum_support_chain_span_ns": 80_000_000,
                 "stable_support_frames_each_side": 2,
             }
             and profile == {
                 "bands": ["X", "K", "Ka"],
                 "directions": ["front", "side", "rear"],
                 "frequency_pattern": "DD.DDD",
                 "meter_cells": 6,
                 "meter_states": ["on", "off", "partial"],
                 "closure_rule": (
                     "per-slot intersection of all refusal-frame compatible counts in the "
                     "bounded context equals the support-derived count"),
                 "interleaved_readable_frames": "exact current secondary context only",
             }
             and record.get("deadline_observation_semantics") ==
                 "LEGAL_PRESENTATION_TRANSITION"
             and spec_document.get("verification_closure_semantics") ==
                 "RAW_CURRENT_BRACKETED_UNRESOLVED_VERIFICATION_BOUNDARY"
             and record.get("verification_closure_semantics") ==
                 spec_document.get("verification_closure_semantics")
             and record.get("auxiliary_closure_context_ns") ==
                 spec_document.get("auxiliary_closure_context_ns") == 80_000_000,
             "temporal secondary-context specification contract differs")

    def card(value: Any) -> bool:
        return (isinstance(value, dict)
                and set(value) == {"band", "frequency", "direction", "bars"}
                and value.get("band") in {"X", "K", "Ka"}
                and isinstance(value.get("frequency"), str)
                and re.fullmatch(r"[0-9]{2}\.[0-9]{3}", value["frequency"]) is not None
                and value.get("direction") in {"front", "side", "rear"}
                and type(value.get("bars")) is int and 0 <= value["bars"] <= 6)

    support_value = record.get("support_derived_secondary")
    _require(isinstance(support_value, list) and 1 <= len(support_value) <= 2
             and all(card(value) for value in support_value)
             and record.get("resolved_value") == support_value,
             "temporal secondary-context support value is malformed")
    signature = record.get("event_signature")
    _require(isinstance(signature, dict)
             and set(signature) == {"mode", "changed_fields", "current_secondary"}
             and signature.get("mode") in {"BASELINE", "UNCHANGED", "CHANGED"}
             and isinstance(signature.get("changed_fields"), list)
             and len(signature["changed_fields"]) == len(set(signature["changed_fields"]))
             and set(signature["changed_fields"]) <= set(FIELDS)
             and signature.get("current_secondary") == support_value,
             "temporal secondary-context event signature is malformed")

    full_indices = record.get("full_context_indices")
    refusals = record.get("context_refusal_indices")
    readable = record.get("interleaved_readable_indices")
    _require(isinstance(full_indices, list) and len(full_indices) >= 5
             and all(type(value) is int for value in full_indices)
             and all(right == left + 1 for left, right in zip(full_indices, full_indices[1:]))
             and isinstance(refusals, list) and bool(refusals)
             and refusals == sorted(set(refusals))
             and isinstance(readable, list) and readable == sorted(set(readable))
             and set(refusals).isdisjoint(readable)
             and set(refusals) | set(readable) == set(full_indices[2:-2])
             and all(value in refusals for value in target_indices)
             and record.get("context_frame_indices") == full_indices,
             "temporal secondary-context indices are malformed")
    _require_point(record.get("context_first"), refusals[0],
                   "secondary context first", source_rows)
    _require_point(record.get("context_last"), refusals[-1],
                   "secondary context last", source_rows)
    supports = record.get("left_support"), record.get("right_support")
    _require(all(isinstance(value, list) and len(value) == 2 for value in supports),
             "temporal secondary-context support record is malformed")
    for point, video_index in zip([*supports[0], *supports[1]],
                                  [*full_indices[:2], *full_indices[-2:]]):
        _require_point(point, video_index, "secondary context support", source_rows)
    established = record.get("current_presentation_established")
    _require_point(established, established.get("video_frame_index")
                   if isinstance(established, dict) else -1,
                   "secondary current-presentation")
    _require(established["capture_ns"] <= source_rows[refusals[0]]["capture_ns"],
             "temporal secondary context precedes current presentation")

    local_gap = min(context["verified_maximum_source_interval_ns"],
                    constants["maximum_support_chain_interval_ns"])
    _validate_temporal_support_chain(full_indices[0], full_indices[-1], source_rows, local_gap)
    _require(source_rows[refusals[-1]]["capture_ns"]
             - source_rows[refusals[0]]["capture_ns"] <=
                 constants["authored_display_update_ns"] + local_gap
             and source_rows[full_indices[-1]]["capture_ns"]
             - source_rows[full_indices[0]]["capture_ns"] <=
                 constants["maximum_support_chain_span_ns"]
             and record.get("maximum_interleaved_readable_frames") ==
                 constants["maximum_interleaved_readable_frames"]
             and record.get("maximum_context_refusal_span_ns") ==
                 constants["authored_display_update_ns"] + local_gap
             and record.get("maximum_support_chain_span_ns") ==
                 constants["maximum_support_chain_span_ns"]
             and record.get("maximum_support_chain_interval_ns") == local_gap,
             "temporal secondary-context timing contract differs")

    def compatible_counts(states: list[str]) -> list[int]:
        return [count for count in range(7) if all(
            state == "partial" or (state == "on" and index < count)
            or (state == "off" and index >= count)
            for index, state in enumerate(states))]

    evidence = record.get("partial_meter_evidence")
    _require(isinstance(evidence, list) and len(evidence) == len(refusals),
             "temporal secondary-context meter evidence is incomplete")
    compatible_by_slot: list[list[set[int]]] = [[] for _ in support_value]
    for item, video_index in zip(evidence, refusals):
        cards = item.get("cards") if isinstance(item, dict) else None
        _require(isinstance(item, dict)
                 and set(item) == {"video_frame_index", "cards"}
                 and item.get("video_frame_index") == video_index
                 and isinstance(cards, list) and len(cards) == len(support_value),
                 "temporal secondary-context meter frame is malformed")
        partial_seen = False
        for slot, (meter, expected) in enumerate(zip(cards, support_value)):
            states = meter.get("cell_states") if isinstance(meter, dict) else None
            meter_state = meter.get("state") if isinstance(meter, dict) else None
            compatible = compatible_counts(states) if (
                isinstance(states, list) and len(states) == 6
                and all(state in {"on", "off", "partial"} for state in states)) else None
            common = (isinstance(meter, dict)
                      and meter.get("slot") == slot
                      and compatible is not None
                      and meter.get("compatible_bars") == compatible
                      and expected["bars"] in compatible)
            if meter_state == "readable":
                valid = (common
                         and set(meter) == {
                             "slot", "state", "bars", "compatible_bars", "cell_states"}
                         and meter.get("bars") == expected["bars"]
                         and compatible == [expected["bars"]]
                         and states == ["on" if index < expected["bars"] else "off"
                                        for index in range(6)])
            else:
                partials = ([index for index, state in enumerate(states)
                             if state == "partial"] if compatible is not None else [])
                valid = (common
                         and set(meter) == {
                             "slot", "state", "bars", "partial_cells",
                             "compatible_bars", "cell_states"}
                         and meter_state == "partial"
                         and meter.get("bars") is None
                         and meter.get("partial_cells") == partials
                         and bool(partials))
                partial_seen = partial_seen or valid
            _require(valid, "temporal secondary-context meter evidence is malformed")
            compatible_by_slot[slot].append(set(compatible))
        _require(partial_seen,
                 "temporal secondary-context refusal contains no partial meter")
    intersections = [sorted(set.intersection(*values)) for values in compatible_by_slot]
    _require(intersections == [[value["bars"]] for value in support_value]
             and record.get("compatible_bar_intersections") == intersections,
             "temporal secondary-context count closure differs")


def _validate_temporal_v2_secondary_optical_record(
        record: dict[str, Any], target_indices: list[int], context: dict[str, Any],
        spec_document: dict[str, Any], source_rows: dict[int, dict[str, int]]) -> None:
    constants = spec_document.get("constants")
    profile = spec_document.get("profile")
    expected_constants = {
        "minimum_ocr_confidence": 0.95,
        "maximum_recording_interval_ns": 1_000_000_000,
        "maximum_support_chain_interval_ns": 10_000_000,
        "maximum_support_chain_span_ns": 25_000_000,
        "stable_support_frames_each_side": 2,
        "profile_rows": 9,
        "profile_columns": 46,
        "profile_channels": 3,
        "profile_byte_count": 1242,
        "maximum_support_pair_rms": 4.0,
        "maximum_support_component_span": 16,
        "maximum_target_support_rms": 4.0,
        "maximum_target_envelope_excursion": 8,
    }
    expected_profile = {
        "boxes": [[440, 377, 621, 413], [687, 377, 868, 413]],
        "rows": 9,
        "columns": 46,
        "channels": ["red", "green", "blue"],
        "order": "row-major cells with RGB-interleaved uint8 components",
        "sample": "rounded arithmetic mean of registered RGB pixels",
        "normalization": "none",
        "encoding": "canonical base64",
    }
    _require(constants == expected_constants and profile == expected_profile
             and record.get("deadline_observation_semantics") ==
                 "LEGAL_PRESENTATION_TRANSITION",
             "temporal secondary optical specification contract differs")
    _require(len(target_indices) == 1,
             "temporal secondary optical record must identify one center frame")

    def card(value: Any) -> bool:
        return (isinstance(value, dict)
                and set(value) == {"band", "frequency", "direction", "bars"}
                and value.get("band") in {"X", "K", "Ka"}
                and isinstance(value.get("frequency"), str)
                and re.fullmatch(r"[0-9]{2}\.[0-9]{3}", value["frequency"]) is not None
                and value.get("direction") in {"front", "side", "rear"}
                and type(value.get("bars")) is int and 0 <= value["bars"] <= 6)

    support = record.get("support_derived_secondary")
    slot = record.get("deficient_slot")
    _require(isinstance(support, list) and 1 <= len(support) <= 2
             and all(card(value) for value in support)
             and record.get("resolved_value") == support
             and type(slot) is int and 0 <= slot < len(support),
             "temporal secondary optical support value or deficient slot is malformed")
    raw_ocr = record.get("raw_frequency_only_ocr")
    confidence = raw_ocr.get("confidence") if isinstance(raw_ocr, dict) else None
    text = raw_ocr.get("text") if isinstance(raw_ocr, dict) else None
    normalized = raw_ocr.get("normalized_frequency") if isinstance(raw_ocr, dict) else None
    _require(isinstance(raw_ocr, dict)
             and set(raw_ocr) == {"text", "normalized_frequency", "confidence"}
             and isinstance(text, str)
             and isinstance(normalized, str)
             and re.fullmatch(r"[0-9]{2}\.[0-9]{3}", normalized) is not None
             and re.sub(r"\s", "", text) == normalized
             and _finite_number(confidence)
             and confidence >= constants["minimum_ocr_confidence"]
             and normalized == support[slot]["frequency"],
             "temporal secondary optical frequency-only OCR evidence is malformed")

    target_index = target_indices[0]
    supports = record.get("left_support"), record.get("right_support")
    _require(all(isinstance(value, list) and len(value) == 2 for value in supports),
             "temporal secondary optical support record is malformed")
    support_indices = [target_index - 2, target_index - 1,
                       target_index + 1, target_index + 2]
    for point, video_index in zip([*supports[0], *supports[1]], support_indices):
        _require_point(point, video_index, "secondary optical support", source_rows)
    local_gap = min(context["verified_maximum_source_interval_ns"],
                    constants["maximum_support_chain_interval_ns"])
    _validate_temporal_support_chain(
        target_index - 2, target_index + 2, source_rows, local_gap)
    _require(source_rows[target_index + 2]["capture_ns"]
             - source_rows[target_index - 2]["capture_ns"] <=
                 constants["maximum_support_chain_span_ns"]
             and record.get("maximum_support_chain_span_ns") ==
                 constants["maximum_support_chain_span_ns"]
             and record.get("maximum_support_chain_interval_ns") == local_gap,
             "temporal secondary optical bracket timing differs")
    established = record.get("current_presentation_established")
    established_index = (established.get("video_frame_index")
                         if isinstance(established, dict) else -1)
    _require_point(established, established_index,
                   "secondary optical current-presentation", source_rows)
    _require(established_index <= target_index
             and established["capture_ns"] <= source_rows[target_index]["capture_ns"],
             "temporal secondary optical refusal precedes current presentation")

    expected_schema = {name: value for name, value in expected_profile.items()
                       if name not in {"boxes", "encoding"}}
    hashes = record.get("profile_sha256s")
    _require(record.get("profile_schema") == expected_schema
             and record.get("profile_reference_bounds") == profile["boxes"][slot]
             and isinstance(hashes, list) and len(hashes) == 5
             and all(isinstance(value, str)
                     and re.fullmatch(r"[0-9a-f]{64}", value) is not None
                     for value in hashes),
             "temporal secondary optical profile identity is malformed")
    expected_limits = {
        "maximum_support_pair_rms": constants["maximum_support_pair_rms"],
        "maximum_support_component_span": constants["maximum_support_component_span"],
        "maximum_target_support_rms": constants["maximum_target_support_rms"],
        "maximum_target_envelope_excursion": constants[
            "maximum_target_envelope_excursion"],
    }
    metrics = record.get("profile_metrics")
    _require(record.get("profile_limits") == expected_limits
             and isinstance(metrics, dict)
             and set(metrics) == {
                 "maximum_support_pair_rms", "maximum_support_component_span",
                 "maximum_target_support_rms", "maximum_target_envelope_excursion",
                 "target_envelope_violation_count"}
             and _finite_number(metrics.get("maximum_support_pair_rms"))
             and 0 <= metrics["maximum_support_pair_rms"] <=
                 expected_limits["maximum_support_pair_rms"]
             and type(metrics.get("maximum_support_component_span")) is int
             and 0 <= metrics["maximum_support_component_span"] <=
                 expected_limits["maximum_support_component_span"]
             and _finite_number(metrics.get("maximum_target_support_rms"))
             and 0 <= metrics["maximum_target_support_rms"] <=
                 expected_limits["maximum_target_support_rms"]
             and type(metrics.get("maximum_target_envelope_excursion")) is int
             and 0 <= metrics["maximum_target_envelope_excursion"] <=
                 expected_limits["maximum_target_envelope_excursion"]
             and type(metrics.get("target_envelope_violation_count")) is int
             and metrics["target_envelope_violation_count"] == 0,
             "temporal secondary optical profile metrics or limits are malformed")
    signature = record.get("event_signature")
    _require(isinstance(signature, dict)
             and set(signature) == {"mode", "changed_fields", "current_secondary"}
             and signature.get("mode") in {"BASELINE", "UNCHANGED", "CHANGED"}
             and isinstance(signature.get("changed_fields"), list)
             and all(isinstance(value, str) for value in signature["changed_fields"])
             and len(signature["changed_fields"]) == len(set(signature["changed_fields"]))
             and set(signature["changed_fields"]) <= set(FIELDS)
             and signature.get("current_secondary") == support,
             "temporal secondary optical event signature is malformed")


def _validate_temporal_v2_fill_record(classifier_id: str, record: dict[str, Any],
                                      target_indices: list[int], context: dict[str, Any],
                                      spec_document: dict[str, Any],
                                      source_rows: dict[int, dict[str, int]]) -> None:
    constants = spec_document["constants"]
    signature = record.get("event_signature")
    _require(isinstance(signature, dict)
             and set(signature) == {"previous_muted_badge", "target_muted_badge",
                                    "stable_primary_frequency", "stable_fields",
                                    "joint_states"}
             and isinstance(signature.get("stable_primary_frequency"), str)
             and re.fullmatch(r"[0-9]{2}\.[0-9]{3}",
                              signature["stable_primary_frequency"]) is not None
             and isinstance(signature.get("stable_fields"), dict)
             and set(signature["stable_fields"]) == set(FIELDS) - {"muted_badge"}
             and all(isinstance(value, dict) and set(value) == {"allowed"}
                     and isinstance(value["allowed"], list) and len(value["allowed"]) == 1
                     for value in signature["stable_fields"].values())
             and signature["stable_fields"]["primary_frequency"]["allowed"] ==
                 [signature["stable_primary_frequency"]]
             and isinstance(signature.get("joint_states"), list)
             and bool(signature["joint_states"]),
             "temporal mute redraw event signature is malformed")
    if classifier_id == "v1-muted-badge-rising-fill-v2":
        _require(signature["previous_muted_badge"] is False
                 and signature["target_muted_badge"] is True,
                 "temporal badge redraw direction is inconsistent")
        full_indices = target_indices
        component_count = spec_document["profile"].get("cells")
    else:
        _require(signature["previous_muted_badge"] is True
                 and signature["target_muted_badge"] is False,
                 "temporal frequency redraw direction is inconsistent")
        full_indices = record.get("full_field_run_indices")
        _require(isinstance(full_indices, list) and bool(full_indices)
                 and all(type(value) is int for value in full_indices)
                 and all(right == left + 1 for left, right in zip(full_indices, full_indices[1:]))
                 and all(value in full_indices for value in target_indices),
                 "temporal frequency full redraw run is invalid")
        _require_point(record.get("full_field_run_first"), full_indices[0],
                       "full field first", source_rows)
        _require_point(record.get("full_field_run_last"), full_indices[-1],
                       "full field last", source_rows)
        masks = [_DIGIT_MASKS[value]
                 for value in signature["stable_primary_frequency"].replace(".", "")]
        _require(record.get("expected_digit_masks") == masks,
                 "temporal frequency glyph masks differ from its stable value")
        component_count = sum(len(mask) for mask in masks)
    first, last = full_indices[0], full_indices[-1]
    supports = record.get("left_support"), record.get("right_support")
    _require(all(isinstance(value, list) and len(value) == 2 for value in supports),
             "temporal redraw support record is malformed")
    for point, index in zip([*supports[0], *supports[1]],
                            (first - 2, first - 1, last + 1, last + 2)):
        _require_point(point, index, "support", source_rows)
    local_gap = min(context["verified_maximum_source_interval_ns"],
                    constants["maximum_support_interval_ns"])
    _validate_temporal_support_chain(first - 2, last + 2, source_rows, local_gap)
    _require(source_rows[last + 2]["capture_ns"] - source_rows[first - 2]["capture_ns"]
             <= constants["maximum_support_chain_span_ns"],
             "temporal redraw support chain exceeds its local span bound")
    separation = record.get("endpoint_component_separation")
    progress = record.get("component_progress")
    totals = record.get("total_backward_motion_by_component")
    _require(type(component_count) is int and component_count > 0
             and isinstance(separation, list) and len(separation) == component_count
             and all(_finite_number(value)
                     and value >= constants["component_separation_min"]
                     for value in separation)
             and isinstance(progress, list) and len(progress) == len(full_indices) + 4
             and all(isinstance(row, list) and len(row) == component_count
                     and all(_finite_number(value)
                             and constants["component_progress_min"] <= value <=
                                 constants["component_progress_max"] for value in row)
                     for row in progress),
             "temporal redraw component metrics are invalid")
    backward_rows = [[max(0.0, before - after) for before, after in zip(left, right)]
                     for left, right in zip(progress, progress[1:])]
    derived_maximum = max((value for row in backward_rows for value in row), default=0.0)
    derived_totals = [sum(row[index] for row in backward_rows)
                      for index in range(component_count)]
    _require(record.get("maximum_backward_step") == derived_maximum
             and derived_maximum <= constants["maximum_backward_step"]
             and record.get("total_backward_motion_by_component") == derived_totals
             and all(value <= constants["maximum_total_backward_motion"]
                     for value in derived_totals)
             and record.get("maximum_support_chain_span_ns") ==
                 constants["maximum_support_chain_span_ns"],
             "temporal redraw component motion is inconsistent")


def _validate_temporal_v2_record(classifier_id: str, spec_sha256: str,
                                 record: dict[str, Any], target_indices: list[int],
                                 context: dict[str, Any], spec_document: dict[str, Any],
                                 decision: str,
                                 source_rows: dict[int, dict[str, int]]) -> None:
    if decision == "REJECTED":
        field = TEMPORAL_V2_OBSERVER_RUBRICS[classifier_id]["raw_affected_fields"][0]
        rejection_keys = (_BRANCHED_REJECTION_RECORD_KEYS
                          if classifier_id == "v1-stable-frequency-intact-context-v1"
                          else _REJECTION_RECORD_KEYS)
        if classifier_id == "v1-arrow-target-acquisition-v1":
            rejection_keys = rejection_keys | {
                "full_transition_indices", "left_support", "right_support"}
        branch_valid = (record.get("branch") == "intact_mask"
                        if classifier_id == "v1-stable-frequency-intact-context-v1" else True)
        _require(set(record) == rejection_keys and branch_valid
                 and isinstance(record.get("event_id"), str) and bool(record["event_id"])
                 and record.get("classifier_id") == classifier_id
                 and record.get("field") == field
                 and record.get("code") in _TEMPORAL_V2_REJECTION_CODES[classifier_id]
                 and isinstance(record.get("reason"), str) and bool(record["reason"]),
                 "temporal rejected classifier record shape differs")
        _require_point(record.get("first"), target_indices[0], "rejected first", source_rows)
        _require_point(record.get("last"), target_indices[-1], "rejected last", source_rows)
        if classifier_id == "v1-arrow-target-acquisition-v1":
            supports = [record.get(side) for side in ("left_support", "right_support")]
            _require(all(isinstance(pair, list) and len(pair) in (0, 2) for pair in supports)
                     and (record["code"] == "UNCLOSED_RUN") == any(not pair for pair in supports),
                     "acquisition rejected support availability differs")
            for pair in supports:
                for point in pair:
                    _require(isinstance(point, dict), "acquisition rejected support is malformed")
                    _require_point(point, point.get("video_frame_index"),
                                   "acquisition rejected support", source_rows)
        return
    required = (_ARROW_RECORD_KEYS if classifier_id == "v1-arrow-phase-edge-v5"
                else _ARROW_ACQUISITION_RECORD_KEYS
                    if classifier_id == "v1-arrow-target-acquisition-v1"
                else _FREQUENCY_CONTEXT_RECORD_KEYS
                    if classifier_id == "v1-stable-frequency-intact-context-v1"
                else _SECONDARY_CONTEXT_RECORD_KEYS
                    if classifier_id == "v1-secondary-closed-context-v3"
                else _SECONDARY_OPTICAL_RECORD_KEYS
                    if classifier_id == "v1-secondary-text-optical-bridge-v1"
                else _BAR_RECORD_KEYS if classifier_id == "v1-main-bar-adjacent-redraw-v2"
                else _BADGE_RECORD_KEYS if classifier_id == "v1-muted-badge-rising-fill-v2"
                else _FREQUENCY_RECORD_KEYS)
    _validate_temporal_v2_common_record(
        classifier_id, spec_sha256, record, target_indices, context, required, source_rows)
    if classifier_id == "v1-arrow-phase-edge-v5":
        _validate_temporal_v2_arrow_record(
            record, target_indices, context, spec_document, source_rows)
    elif classifier_id == "v1-arrow-target-acquisition-v1":
        _validate_temporal_v2_arrow_acquisition_record(
            record, target_indices, context, spec_document, source_rows)
    elif classifier_id == "v1-stable-frequency-intact-context-v1":
        _validate_temporal_v2_frequency_context_record(
            record, target_indices, context, spec_document, source_rows)
    elif classifier_id == "v1-secondary-closed-context-v3":
        _validate_temporal_v2_secondary_context_record(
            record, target_indices, context, spec_document, source_rows)
    elif classifier_id == "v1-secondary-text-optical-bridge-v1":
        _validate_temporal_v2_secondary_optical_record(
            record, target_indices, context, spec_document, source_rows)
    elif classifier_id == "v1-main-bar-adjacent-redraw-v2":
        _validate_temporal_v2_bar_record(
            record, target_indices, context, spec_document, source_rows)
    else:
        _validate_temporal_v2_fill_record(
            classifier_id, record, target_indices, context, spec_document, source_rows)


def _temporal_v2_frequency_context_band(
        record: dict[str, Any], analysis_result: dict[str, Any]) -> str:
    """Derive the admitted primary band from the retained input event, not observer data."""
    sequence = analysis_result.get("sequence")
    events = sequence.get("events") if isinstance(sequence, dict) else None
    _require(isinstance(events, list) and all(isinstance(event, dict) for event in events),
             "retained frequency-context events are malformed")
    matching = [event for event in events if event.get("event_id") == record.get("event_id")]
    _require(len(matching) == 1, "frequency-context record event identity is ambiguous")
    event = matching[0]
    rows = event.get("wire_rows")
    primaries = ([row for row in rows
                  if isinstance(row, dict) and row.get("priority") is True]
                 if isinstance(rows, list) else [])
    _require(len(primaries) == 1, "frequency-context event lacks one primary input row")
    primary = primaries[0]
    raw_band = primary.get("band")
    # Sequence wire_rows are normalized by encounter_expectation._row; the
    # scenario document's integer frequencyMHz does not survive in this layer.
    frequency = primary.get("frequency")
    _require(isinstance(raw_band, str) and raw_band.casefold() in {"x", "k", "ka"}
             and isinstance(frequency, str)
             and re.fullmatch(r"[0-9]{2}\.[0-9]{3}", frequency) is not None
             and 0 < int(frequency.replace(".", "")) <= 65_535,
             "frequency-context primary input identity is malformed")
    band = {"x": "X", "k": "K", "ka": "Ka"}[raw_band.casefold()]
    signature = record.get("event_signature")
    target = event.get("target")
    fields = target.get("fields") if isinstance(target, dict) else None
    frequency_target = fields.get("primary_frequency") if isinstance(fields, dict) else None
    _require(record.get("support_derived_frequency") == frequency
             and isinstance(signature, dict)
             and signature.get("mode") == event.get("mode")
             and signature.get("changed_fields") == event.get("changed_fields")
             and signature.get("current_primary_frequency") == frequency
             and isinstance(frequency_target, dict)
             and frequency_target.get("allowed") == [frequency],
             "frequency-context record differs from its retained primary event")
    return band


def _validate_temporal_v2_secondary_context_event(
        record: dict[str, Any], analysis_result: dict[str, Any]) -> None:
    """Bind post-acquisition secondary evidence to the retained event boundary."""
    sequence = analysis_result.get("sequence")
    events = sequence.get("events") if isinstance(sequence, dict) else None
    _require(isinstance(events, list) and all(isinstance(event, dict) for event in events),
             "retained secondary-context events are malformed")
    matching = [event for event in events if event.get("event_id") == record.get("event_id")]
    _require(len(matching) == 1, "secondary-context record event identity is ambiguous")
    event = matching[0]
    first_correct = event.get("first_correct")
    established = record.get("current_presentation_established")
    point_keys = {
        "frame_id", "video_frame_index", "source_frame_seq", "capture_ns",
        "offset_seconds", "image"}
    signature = record.get("event_signature")
    target = event.get("target")
    fields = target.get("fields") if isinstance(target, dict) else None
    secondary_target = fields.get("secondary") if isinstance(fields, dict) else None
    _require(isinstance(first_correct, dict)
             and set(first_correct) <= point_keys
             and isinstance(established, dict)
             and all(established.get(name) == value
                     for name, value in first_correct.items())
             and isinstance(signature, dict)
             and signature.get("mode") == event.get("mode")
             and signature.get("changed_fields") == event.get("changed_fields")
             and isinstance(secondary_target, dict)
             and secondary_target.get("allowed") == [record.get("support_derived_secondary")],
             "secondary-context record differs from its retained current event")


def _temporal_v2_secondary_optical_band(
        record: dict[str, Any], analysis_result: dict[str, Any]) -> str:
    """Bind the optical bridge and its coverage band to one retained current event."""
    sequence = analysis_result.get("sequence")
    events = sequence.get("events") if isinstance(sequence, dict) else None
    _require(isinstance(events, list) and all(isinstance(event, dict) for event in events),
             "retained secondary optical events are malformed")
    matching = [event for event in events if event.get("event_id") == record.get("event_id")]
    _require(len(matching) == 1, "secondary optical record event identity is ambiguous")
    event = matching[0]
    first_correct = event.get("first_correct")
    established = record.get("current_presentation_established")
    point_keys = {
        "frame_id", "video_frame_index", "source_frame_seq", "capture_ns",
        "offset_seconds", "image", "image_sha256"}
    signature = record.get("event_signature")
    support = record.get("support_derived_secondary")
    slot = record.get("deficient_slot")
    target = event.get("target")
    fields = target.get("fields") if isinstance(target, dict) else None
    secondary_target = fields.get("secondary") if isinstance(fields, dict) else None
    _require(isinstance(first_correct, dict) and bool(first_correct)
             and set(first_correct) <= point_keys
             and isinstance(established, dict)
             and all(established.get(name) == value
                     for name, value in first_correct.items())
             and isinstance(signature, dict)
             and signature.get("mode") == event.get("mode")
             and signature.get("changed_fields") == event.get("changed_fields")
             and signature.get("current_secondary") == support
             and isinstance(secondary_target, dict)
             and secondary_target.get("allowed") == [support]
             and isinstance(support, list)
             and type(slot) is int and 0 <= slot < len(support),
             "secondary optical record differs from its retained current event")
    band = support[slot].get("band")
    _require(band in {"X", "K", "Ka"},
             "secondary optical deficient-slot band is malformed")
    return band


def _validate_temporal_v2(document: dict[str, Any], classifier_id: str,
                          spec_sha256: str, evidence_root: Path,
                          entry: dict[str, Any], implementation: dict[str, str],
                          reader: dict[str, Any], bench_source_sha256: str | None,
                          spec_document: Any) -> dict[str, Any]:
    _require(classifier_id in TEMPORAL_V2_OBSERVER_RUBRICS,
             f"unsupported generic temporal classifier: {classifier_id}")
    _require(document.get("classifier_id") == classifier_id
             and document.get("classifier_spec_sha256") == spec_sha256,
             f"temporal validation identity differs for {classifier_id}")
    spec_validation = (spec_document.get("validation")
                       if isinstance(spec_document, dict) else None)
    if classifier_id == "v1-arrow-phase-edge-v5":
        requirements = (spec_document.get("qualification_requirements")
                        if isinstance(spec_document, dict) else None)
        valid_specification = (
            isinstance(spec_document, dict)
            and spec_document.get("schema_version") == 2
            and spec_document.get("classifier_id") == classifier_id
            and spec_document.get("deadline_observation_semantics") ==
                "LEGAL_PRESENTATION_TRANSITION"
            and requirements == {
                "minimum_blind_true_admits": MINIMUM_TEMPORAL_POSITIVES,
                "minimum_blind_true_rejects": MINIMUM_TEMPORAL_NEGATIVES,
                "maximum_false_admits": 0,
            })
    elif classifier_id == "v1-arrow-target-acquisition-v1":
        valid_specification = (
            isinstance(spec_document, dict)
            and spec_document.get("schema_version") == 1
            and spec_document.get("classifier_id") == classifier_id
            and spec_document.get("deadline_observation_semantics") ==
                "TARGET_ACQUISITION_TRANSITION"
            and spec_document.get("scope", {}).get("field") == "main_arrows"
            and isinstance(spec_validation, dict)
            and spec_validation.get("minimum_blind_true_admits") ==
                MINIMUM_TEMPORAL_POSITIVES
            and spec_validation.get("minimum_blind_true_rejects") ==
                MINIMUM_TEMPORAL_NEGATIVES
            and spec_validation.get("required_false_admits") == 0
            and spec_validation.get("observer_eligibility_rule") ==
                TEMPORAL_V2_OBSERVER_RUBRICS[classifier_id]["observer_eligibility_rule"])
    elif classifier_id == "v1-secondary-text-optical-bridge-v1":
        valid_specification = (
            isinstance(spec_document, dict)
            and spec_document.get("schema_version") == 1
            and spec_document.get("classifier_id") == classifier_id
            and spec_document.get("deadline_observation_semantics") ==
                "LEGAL_PRESENTATION_TRANSITION"
            and spec_document.get("scope", {}).get("field") == "secondary"
            and spec_document.get("scope", {}).get("minimum_cards") == 1
            and spec_document.get("scope", {}).get("maximum_cards") == 2
            and spec_validation == {
                "minimum_blind_true_admits": MINIMUM_TEMPORAL_POSITIVES,
                "minimum_blind_true_rejects": MINIMUM_TEMPORAL_NEGATIVES,
                "observer_eligibility_rule":
                    TEMPORAL_V2_OBSERVER_RUBRICS[classifier_id][
                        "observer_eligibility_rule"],
                "required_band_coverage": ["X", "K", "Ka"],
                "required_false_admits": 0,
            })
    elif classifier_id == "v1-secondary-closed-context-v3":
        valid_specification = (
            isinstance(spec_document, dict)
            and spec_document.get("schema_version") == 1
            and spec_document.get("classifier_id") == classifier_id
            and spec_document.get("deadline_observation_semantics") ==
                "LEGAL_PRESENTATION_TRANSITION"
            and spec_document.get("verification_closure_semantics") ==
                "RAW_CURRENT_BRACKETED_UNRESOLVED_VERIFICATION_BOUNDARY"
            and spec_document.get("auxiliary_closure_context_ns") == 80_000_000
            and spec_document.get("scope", {}).get("field") == "secondary"
            and spec_document.get("scope", {}).get("minimum_cards") == 1
            and spec_document.get("scope", {}).get("maximum_cards") == 2
            and spec_validation == {
                "minimum_blind_true_admits": MINIMUM_TEMPORAL_POSITIVES,
                "minimum_blind_true_rejects": MINIMUM_TEMPORAL_NEGATIVES,
                "observer_eligibility_rule":
                    TEMPORAL_V2_OBSERVER_RUBRICS[classifier_id][
                        "observer_eligibility_rule"],
                "required_false_admits": 0,
            })
    elif classifier_id == "v1-stable-frequency-intact-context-v1":
        branch_gate = {
            "minimum_blind_true_admits": MINIMUM_TEMPORAL_POSITIVES,
            "minimum_blind_true_rejects": MINIMUM_TEMPORAL_NEGATIVES,
            "required_false_admits": 0,
        }
        valid_specification = (
            isinstance(spec_document, dict)
            and spec_document.get("schema_version") == 1
            and spec_document.get("classifier_id") == classifier_id
            and spec_document.get("deadline_observation_semantics") ==
                "LEGAL_PRESENTATION_TRANSITION"
            and spec_document.get("verification_closure_semantics") ==
                "RAW_CURRENT_BRACKETED_UNRESOLVED_VERIFICATION_BOUNDARY"
            and spec_document.get("scope", {}).get("field") == "primary_frequency"
            and isinstance(spec_validation, dict)
            and spec_validation.get("branch_gates") == {
                "intact_mask": branch_gate,
            }
            and spec_validation.get("required_band_coverage") == ["X", "K", "Ka"]
            and spec_validation.get("observer_eligibility_rule") ==
                TEMPORAL_V2_OBSERVER_RUBRICS[classifier_id]["observer_eligibility_rule"])
    else:
        valid_specification = (
            isinstance(spec_document, dict)
            and spec_document.get("schema_version") == 1
            and spec_document.get("classifier_id") == classifier_id
            and isinstance(spec_validation, dict)
            and spec_validation.get("minimum_blind_true_admits") ==
                MINIMUM_TEMPORAL_POSITIVES
            and spec_validation.get("minimum_blind_true_rejects") ==
                MINIMUM_TEMPORAL_NEGATIVES
            and spec_validation.get("required_false_admits") == 0
            and spec_validation.get("observer_eligibility_rule") ==
                TEMPORAL_V2_OBSERVER_RUBRICS[classifier_id]["observer_eligibility_rule"])
    _require(valid_specification,
             f"temporal specification validation contract differs for {classifier_id}")

    implementation_binding = {
        name: _digest(implementation.get(name), f"{classifier_id} implementation {name}")
        for name in CLASSIFIER_IMPLEMENTATION_FILES[classifier_id]
    }
    reader_binding = {
        "method_version": reader.get("method_version"),
        "source_sha256": _digest(
            implementation.get("encounter_reader.py"), f"{classifier_id} reader source"),
        "runtime_sha256": _canonical_sha256(reader),
        "bench_source_sha256": _digest(bench_source_sha256, "running bench.sh"),
    }
    rubric_sha256 = _canonical_sha256(TEMPORAL_V2_OBSERVER_RUBRICS[classifier_id])

    references = entry.get("source_artifacts")
    _require(isinstance(references, dict) and set(references) == set(TEMPORAL_V2_SOURCE_NAMES),
             f"temporal source artifacts are incomplete for {classifier_id}")
    declared_hashes = document.get("source_artifacts")
    _require(isinstance(declared_hashes, dict)
             and set(declared_hashes) == set(TEMPORAL_SOURCE_HASH_FIELDS.values()),
             f"temporal validation has incomplete source provenance for {classifier_id}")
    source_paths: dict[str, Path] = {}
    sources: dict[str, Any] = {}
    for name in TEMPORAL_V2_SOURCE_NAMES:
        field = TEMPORAL_SOURCE_HASH_FIELDS[name]
        _require(isinstance(references[name], dict),
                 f"temporal source reference is malformed for {classifier_id}: {name}")
        _require(references[name].get("sha256") == declared_hashes.get(field),
                 f"temporal source hash differs for {classifier_id}: {name}")
        if name in {"observer_readme", "qualification_video", "frame_timing"}:
            source_paths[name] = _evidence_file(
                evidence_root, references[name], f"{classifier_id} {name}")
        else:
            source_paths[name], sources[name] = _evidence(
                evidence_root, references[name], f"{classifier_id} {name}")
    _require(all(isinstance(sources.get(name), dict)
                 for name in TEMPORAL_V2_SOURCE_NAMES
                 if name not in {"observer_readme", "qualification_video", "frame_timing"}),
             f"temporal source document is malformed for {classifier_id}")

    seal = sources["seal"]
    for name in ("pre_pixel_freeze", "frozen_classifier_result", "observer_manifest",
                 "observer_readme", "restricted_hidden_key", "selection",
                 "analysis_selection", "qualification_capture", "window_result",
                 "capture_manifest", "qualification_video", "frame_timing",
                 "video_timing_verification", "analysis_result"):
        field = TEMPORAL_SOURCE_HASH_FIELDS[name]
        _require(seal.get(field) == references[name].get("sha256"),
                 f"temporal seal differs for {classifier_id}: {name}")
    _require(seal.get("classifier_id") == classifier_id
             and seal.get("classifier_spec_sha256") == spec_sha256
             and seal.get("classifier_implementation_sha256") == implementation_binding
             and seal.get("reader_binding") == reader_binding
             and seal.get("observer_rubric_sha256") == rubric_sha256
             and seal.get("allowlist_status") == "NOT_ALLOWLISTED",
             f"temporal pre-adjudication seal is invalid for {classifier_id}")

    pre_pixel = sources["pre_pixel_freeze"]
    hidden = sources["restricted_hidden_key"]
    frozen = sources["frozen_classifier_result"]
    _validate_temporal_v2_capture(
        sources["qualification_capture"], sources["window_result"],
        source_paths["qualification_capture"], source_paths["window_result"],
        references, bench_source_sha256)
    context_binding = _temporal_v2_context_binding(
        classifier_id, spec_document, sources["window_result"], references,
        implementation, reader)
    _require(pre_pixel.get("allowlist_status") == "NOT_ALLOWLISTED"
             and pre_pixel.get("classifier") == {
                 "id": classifier_id,
                 "spec_sha256": spec_sha256,
                 "implementation_sha256": implementation_binding,
             }
             and pre_pixel.get("reader_binding") == reader_binding
             and pre_pixel.get("observer_rubric_sha256") == rubric_sha256
             and pre_pixel.get("candidate_selection") == TEMPORAL_CANDIDATE_SELECTION,
             f"temporal pre-pixel freeze differs for {classifier_id}")
    _require(hidden.get("do_not_provide_to_observer") is True
             and hidden.get("classifier_id") == classifier_id
             and hidden.get("classifier_spec_sha256") == spec_sha256
             and hidden.get("allowlist_status") ==
                 "NOT_ALLOWLISTED_PENDING_BLIND_ADJUDICATION",
             f"temporal hidden key differs for {classifier_id}")
    _require(frozen.get("classifier_id") == classifier_id
             and frozen.get("classifier_spec_sha256") == spec_sha256
             and frozen.get("classifier_implementation_sha256") == implementation_binding
             and frozen.get("reader_binding") == reader_binding
             and frozen.get("observer_rubric_sha256") == rubric_sha256
             and frozen.get("classifier_context") == context_binding
             and frozen.get("pre_pixel_freeze_sha256") ==
                 references["pre_pixel_freeze"].get("sha256")
             and frozen.get("selection_sha256") == references["selection"].get("sha256")
             and frozen.get("errors") == [],
             f"temporal frozen classifier result differs for {classifier_id}")

    observer_manifest = sources["observer_manifest"]
    observations = sources["completed_observations"]
    blind_protocol = {
        "observations_completed_before_key_access": True,
        "observer_received_machine_output": False,
        "observer_received_hidden_key": False,
    }
    _require(set(observer_manifest) == {
                 "schema_version", "kind", "classifier_id", "classifier_spec_sha256",
                 "observer_rubric_sha256", "blind_protocol", "item_count", "items"}
             and observer_manifest.get("schema_version") == 2
             and observer_manifest.get("kind") == "blind_temporal_observer_manifest"
             and observer_manifest.get("classifier_id") == classifier_id
             and observer_manifest.get("classifier_spec_sha256") == spec_sha256
             and observer_manifest.get("blind_protocol") == blind_protocol,
             f"temporal observer manifest differs for {classifier_id}")
    manifest_items = observer_manifest.get("items")
    observation_items = observations.get("observations")
    hidden_items = hidden.get("items")
    comparisons = document.get("comparisons")
    record_lists = (manifest_items, observation_items, hidden_items, comparisons)
    _require(all(isinstance(value, list) and all(isinstance(item, dict) for item in value)
                 for value in record_lists),
             f"temporal blind records are malformed for {classifier_id}")
    ids = [[item.get("opaque_id") for item in values] for values in record_lists]
    _require(all(all(isinstance(value, str) and value for value in values)
                 and len(values) == len(set(values)) for values in ids)
             and ids[0] == ids[1] == ids[2] == ids[3]
             and observer_manifest.get("item_count") == len(ids[0]),
             f"temporal blind record identities differ for {classifier_id}")
    selection = sources["selection"]
    frozen_admitted = frozen.get("classifications")
    frozen_rejected = frozen.get("rejected_runs")
    _require(all(isinstance(values, list) and all(isinstance(item, dict) for item in values)
                 for values in (frozen_admitted, frozen_rejected)),
             f"temporal frozen candidate inventory is malformed for {classifier_id}")
    _require(selection == temporal_selection_document(
                 len(frozen_admitted), len(frozen_rejected), ids[0])
             and len(ids[0]) == sum(selection["selected_counts"].values()),
             f"temporal selection differs from the frozen candidate set for {classifier_id}")
    analysis_selection = sources["analysis_selection"]
    analysis_samples = analysis_selection.get("samples")
    _require(analysis_selection.get("schema_version") == 1
             and isinstance(analysis_selection.get("identity"), dict)
             and analysis_selection["identity"].get("capture_id") ==
                 context_binding["capture_id"]
             and isinstance(analysis_samples, list)
             and all(isinstance(item, dict)
                     and type(item.get("video_frame_index")) is int
                     and type(item.get("source_frame_seq")) is int
                     and type(item.get("capture_ns")) is int
                     for item in analysis_samples),
             f"temporal analysis selection is malformed for {classifier_id}")
    analysis_indices = [item["video_frame_index"] for item in analysis_samples]
    _require(len(analysis_indices) == len(set(analysis_indices)),
             f"temporal analysis selection duplicates source frames for {classifier_id}")
    _require(observations.get("instructions_sha256") ==
             references["observer_readme"].get("sha256")
             and observations.get("observer_rubric_sha256") == rubric_sha256
             and observer_manifest.get("observer_rubric_sha256") == rubric_sha256
             and observations.get("blind_protocol") == blind_protocol,
             f"temporal blind instructions differ for {classifier_id}")
    try:
        retained_instructions = source_paths["observer_readme"].read_text(encoding="utf-8")
    except (OSError, UnicodeError) as exc:
        raise QualificationError(
            f"temporal blind instructions are unreadable for {classifier_id}") from exc
    _require(retained_instructions == temporal_v2_observer_instructions(classifier_id),
             f"temporal blind instructions are not the code-owned rubric for {classifier_id}")

    source_rows = _validate_temporal_v2_media(
        classifier_id, source_paths, sources, sources["window_result"],
        analysis_selection, manifest_items)

    admitted_records = [item.get("frozen_classifier_record") for item in hidden_items
                        if item.get("frozen_classifier_decision") == "ADMITTED"]
    rejected_records = [item.get("frozen_classifier_record") for item in hidden_items
                        if item.get("frozen_classifier_decision") == "REJECTED"]
    canonical = lambda values: Counter(
        json.dumps(value, sort_keys=True, separators=(",", ":")) for value in values)
    _require(canonical(admitted_records) == canonical(frozen_admitted)
             and canonical(rejected_records) == canonical(select_temporal_rejections(
                 classifier_id, context_binding["capture_id"], frozen_rejected)),
             f"temporal hidden decisions differ from frozen classifier output for {classifier_id}")
    analysis_result = sources["analysis_result"]
    analysis_temporal = analysis_result.get("temporal_classification")
    analysis_evidence = analysis_result.get("evidence")
    _require(isinstance(analysis_temporal, dict) and analysis_temporal.get("errors") == []
             and isinstance(analysis_evidence, dict)
             and analysis_evidence.get("selection_manifest_sha256") ==
                 references["analysis_selection"].get("sha256"),
             f"retained analyzer result differs for {classifier_id}")
    analysis_admitted = [
        value for value in analysis_temporal.get("classifications", [])
        if isinstance(value, dict) and value.get("classifier_id") == classifier_id]
    field = TEMPORAL_V2_OBSERVER_RUBRICS[classifier_id]["raw_affected_fields"][0]
    all_analysis_rejected = analysis_temporal.get("rejected_runs", [])
    _require(isinstance(all_analysis_rejected, list)
             and all(isinstance(value, dict)
                     and isinstance(value.get("classifier_id"), str)
                     and bool(value["classifier_id"])
                     for value in all_analysis_rejected),
             f"retained analyzer rejections lack classifier provenance for {classifier_id}")
    analysis_rejected = [
        value for value in all_analysis_rejected
        if value.get("classifier_id") == classifier_id and value.get("field") == field]
    _require(canonical(analysis_admitted) == canonical(frozen.get("classifications", []))
             and canonical(analysis_rejected) == canonical(frozen.get("rejected_runs", [])),
             f"frozen classifier output differs from retained analyzer result for {classifier_id}")

    integrity = document.get("integrity")
    checks = integrity.get("checks") if isinstance(integrity, dict) else None
    clip_checks = integrity.get("clip_checks") if isinstance(integrity, dict) else None
    _require(isinstance(checks, dict)
             and set(checks) == set(TEMPORAL_V2_INTEGRITY_CHECKS)
             and all(checks[name] is True for name in TEMPORAL_V2_INTEGRITY_CHECKS)
             and isinstance(clip_checks, list)
             and all(isinstance(item, dict) and set(item) == {
                 "opaque_id", "actual_sha256", "hidden_key_sha256", "sealed_sha256",
                 "actual_size_bytes", "sealed_size_bytes", "target_run_video_indices",
                 "clip_source_video_indices", "target_run_clip_frame_indices",
                 "path_matches_id", "source_frame_indices_match_target_run",
                 "target_run_inside_clip"} for item in clip_checks),
             f"temporal integrity record failed for {classifier_id}")
    clip_by_id = {item.get("opaque_id"): item for item in clip_checks}
    _require(len(clip_by_id) == len(clip_checks) and set(clip_by_id) == set(ids[0]),
             f"temporal clip audit identities differ for {classifier_id}")

    outcomes: Counter[str] = Counter()
    outcome_ids = {name: [] for name in
                   ("true_admit", "false_admit", "true_reject", "false_reject",
                    "abstain")}
    ground_truth_counts: Counter[str] = Counter()
    frequency_branches = ("intact_mask",)
    branch_outcomes = ({branch: Counter() for branch in frequency_branches}
                       if classifier_id == "v1-stable-frequency-intact-context-v1"
                       else {})
    true_admit_bands: set[str] = set()
    retained_clip_paths: set[Path] = set()
    retained_clip_hashes: set[str] = set()
    retained_candidates: set[tuple[Any, ...]] = set()
    for manifest_item, observation, hidden_item, comparison in zip(
            manifest_items, observation_items, hidden_items, comparisons):
        opaque_id = comparison["opaque_id"]
        _require(set(manifest_item) == {
                     "opaque_id", "clip", "sha256", "size_bytes",
                     "target_run_video_indices", "clip_source_video_indices",
                     "target_run_clip_frame_indices", "inset_source_box"} |
                     (ACQUISITION_OBSERVER_SCOPE_FIELDS
                      if classifier_id == "v1-arrow-target-acquisition-v1" else set()),
                 f"temporal observer manifest item differs for {opaque_id}")
        manifest_sha = _digest(manifest_item.get("sha256"), f"temporal clip {opaque_id}")
        _require(hidden_item.get("clip_sha256") == manifest_sha,
                 f"temporal hidden clip hash differs for {opaque_id}")
        clip_path = _evidence_file(source_paths["observer_manifest"].parent,
                                   {"path": manifest_item.get("clip"),
                                    "sha256": manifest_sha},
                                   f"temporal clip {opaque_id}")
        clip_relative = manifest_item.get("clip")
        _require(isinstance(clip_relative, str)
                 and Path(clip_relative).parent == Path("clips")
                 and Path(clip_relative).stem == opaque_id
                 and bool(Path(clip_relative).suffix),
                 f"temporal clip path does not match its opaque id: {opaque_id}")
        _require(type(manifest_item.get("size_bytes")) is int
                 and clip_path.stat().st_size == manifest_item["size_bytes"],
                 f"temporal clip size differs for {opaque_id}")
        _require(clip_path not in retained_clip_paths and manifest_sha not in retained_clip_hashes,
                 f"temporal clip evidence is duplicated for {opaque_id}")
        retained_clip_paths.add(clip_path)
        retained_clip_hashes.add(manifest_sha)

        target_indices = hidden_item.get("target_run_video_indices")
        _require(isinstance(target_indices, list) and bool(target_indices)
                 and all(type(value) is int and value >= 0 for value in target_indices)
                 and all(right == left + 1
                         for left, right in zip(target_indices, target_indices[1:])),
                 f"temporal target indices are invalid for {opaque_id}")
        _require(all(value in set(analysis_indices) for value in target_indices),
                 f"temporal target indices are absent from analysis selection for {opaque_id}")
        _require(manifest_item.get("target_run_video_indices") == target_indices,
                 f"temporal manifest target indices differ for {opaque_id}")
        clip_source_indices = manifest_item.get("clip_source_video_indices")
        target_clip_indices = manifest_item.get("target_run_clip_frame_indices")
        full_run_indices = hidden_item.get("full_run_video_indices")
        full_run_clip_indices = hidden_item.get("full_run_clip_frame_indices")
        scope = (temporal_acquisition_observer_scope(manifest_item)
                 if classifier_id == "v1-arrow-target-acquisition-v1" else None)
        expected_clip_source_indices = temporal_observer_clip_source_indices(
            target_indices, source_rows,
            [index for values in scope.values() for index in values] if scope is not None else None)
        _require(isinstance(clip_source_indices, list) and bool(clip_source_indices)
                 and all(type(value) is int and value >= 0 for value in clip_source_indices)
                 and all(right == left + 1
                         for left, right in zip(clip_source_indices, clip_source_indices[1:])),
                 f"temporal clip source mapping is invalid for {opaque_id}")
        _require(clip_source_indices == expected_clip_source_indices,
                 f"temporal observer clip does not use the fixed target context for {opaque_id}")
        expected_target_clip_indices = [
            offset for offset, source_index in enumerate(clip_source_indices)
            if source_index in set(target_indices)
        ]
        _require(isinstance(full_run_indices, list) and bool(full_run_indices)
                 and all(type(value) is int and value >= 0 for value in full_run_indices)
                 and all(right == left + 1
                         for left, right in zip(full_run_indices, full_run_indices[1:])),
                 f"temporal full-run source mapping is invalid for {opaque_id}")
        expected_full_run_clip_indices = [
            offset for offset, source_index in enumerate(clip_source_indices)
            if source_index in set(full_run_indices)
        ]
        _require(target_clip_indices == expected_target_clip_indices
                 and len(target_clip_indices) == len(target_indices)
                 and [clip_source_indices[offset] for offset in target_clip_indices] ==
                     target_indices
                 and hidden_item.get("clip_source_video_indices") == clip_source_indices
                 and hidden_item.get("target_run_clip_frame_indices") == target_clip_indices,
                 f"temporal clip target mapping differs for {opaque_id}")
        _require(full_run_clip_indices == expected_full_run_clip_indices
                 and len(full_run_clip_indices) == len(full_run_indices)
                 and [clip_source_indices[offset] for offset in full_run_clip_indices] ==
                     full_run_indices
                 and all(value in full_run_indices for value in target_indices)
                 and all(value in clip_source_indices for value in full_run_indices),
                 f"temporal clip full-run mapping differs for {opaque_id}")
        clip_audit = clip_by_id[opaque_id]
        _require(clip_audit.get("actual_sha256") == manifest_sha
                 and clip_audit.get("hidden_key_sha256") == manifest_sha
                 and clip_audit.get("sealed_sha256") == manifest_sha
                 and clip_audit.get("actual_size_bytes") == manifest_item["size_bytes"]
                 and clip_audit.get("sealed_size_bytes") == manifest_item["size_bytes"]
                 and clip_audit.get("target_run_video_indices") == target_indices
                 and clip_audit.get("clip_source_video_indices") == clip_source_indices
                 and clip_audit.get("target_run_clip_frame_indices") == target_clip_indices
                 and all(clip_audit.get(name) is True for name in
                         ("path_matches_id", "source_frame_indices_match_target_run",
                          "target_run_inside_clip")),
                 f"temporal clip audit differs for {opaque_id}")

        if classifier_id == "v1-arrow-target-acquisition-v1":
            temporal_acquisition_observer_scope(manifest_item, observation)
        literal, ground_truth = _temporal_v2_observer_ground_truth(
            classifier_id, observation)
        eligible = ground_truth == "ELIGIBLE"
        ground_truth_counts[ground_truth] += 1
        decision = hidden_item.get("frozen_classifier_decision")
        _require(decision in ("ADMITTED", "REJECTED"),
                 f"temporal frozen decision is invalid for {opaque_id}")
        record = hidden_item.get("frozen_classifier_record")
        _require(set(hidden_item) == {
                     "opaque_id", "clip_sha256", "frozen_classifier_decision",
                     "frozen_classifier_record", "frozen_classifier_record_sha256",
                     "target_run_video_indices", "clip_source_video_indices",
                     "target_run_clip_frame_indices", "full_run_video_indices",
                     "full_run_clip_frame_indices"},
                 f"temporal hidden item shape differs for {opaque_id}")
        _require(isinstance(record, dict), f"temporal frozen record is missing for {opaque_id}")
        record_sha = _canonical_sha256(record)
        _require(hidden_item.get("frozen_classifier_record_sha256") == record_sha,
                 f"temporal frozen record hash differs for {opaque_id}")
        _validate_temporal_v2_record(
            classifier_id, spec_sha256, record, target_indices, context_binding,
            spec_document, decision, source_rows)
        if classifier_id == "v1-arrow-target-acquisition-v1":
            _validate_temporal_v2_acquisition_scope_binding(
                manifest_item, record, full_run_indices)
        admitted_band = None
        if decision == "ADMITTED":
            _require(record.get("video_frame_indices") == target_indices,
                     f"temporal admitted record target indices differ for {opaque_id}")
            claim_matches = _temporal_v2_claim_matches_record(
                classifier_id, spec_sha256, literal, record)
            if classifier_id == "v1-secondary-closed-context-v3":
                _validate_temporal_v2_secondary_context_event(record, analysis_result)
            elif classifier_id == "v1-secondary-text-optical-bridge-v1":
                _require(full_run_indices == list(range(
                             target_indices[0] - 2, target_indices[0] + 3)),
                         f"temporal secondary optical five-frame bracket differs for {opaque_id}")
                admitted_band = _temporal_v2_secondary_optical_band(
                    record, analysis_result)
        else:
            first = record.get("first")
            last = record.get("last")
            _require(isinstance(first, dict) and isinstance(last, dict)
                     and first.get("video_frame_index") == target_indices[0]
                     and last.get("video_frame_index") == target_indices[-1]
                     and record.get("field") ==
                         TEMPORAL_V2_OBSERVER_RUBRICS[classifier_id][
                             "raw_affected_fields"][0],
                     f"temporal rejected record target indices differ for {opaque_id}")
            claim_matches = None
        field = TEMPORAL_V2_OBSERVER_RUBRICS[classifier_id]["raw_affected_fields"][0]
        candidate = (field, tuple(target_indices))
        _require(isinstance(record.get("event_id"), str) and record["event_id"]
                 and candidate not in retained_candidates,
                 f"temporal source candidate is duplicated for {opaque_id}")
        retained_candidates.add(candidate)

        if decision == "ADMITTED":
            outcome = "TRUE_ADMIT" if eligible and claim_matches else "FALSE_ADMIT"
        elif eligible:
            outcome = "FALSE_REJECT"
        elif ground_truth == "DEFINITE_NEGATIVE":
            outcome = "TRUE_REJECT"
        else:
            outcome = "ABSTAIN"
        key = outcome.casefold()
        _require(comparison.get("observer_literal") == literal
                 and comparison.get("observer_strict_visual_eligible_for_admission") is eligible
                 and comparison.get("observer_ground_truth") == ground_truth
                 and comparison.get("frozen_classifier_decision") == decision
                 and comparison.get("frozen_rejection_code") ==
                     (None if decision == "ADMITTED" else record.get("code"))
                 and comparison.get("frozen_classifier_record_sha256") == record_sha
                 and comparison.get("observer_claim_matches_frozen_record") is claim_matches
                 and comparison.get("source_video_frame_indices") == target_indices
                 and comparison.get("comparison_outcome") == outcome,
                 f"temporal comparison was not independently derived for {opaque_id}")
        outcomes[key] += 1
        outcome_ids[key].append(opaque_id)
        if outcome == "TRUE_ADMIT" and admitted_band is not None:
            true_admit_bands.add(admitted_band)
        if branch_outcomes:
            branch = record["branch"]
            branch_outcomes[branch][key] += 1
            if outcome == "TRUE_ADMIT":
                true_admit_bands.add(
                    _temporal_v2_frequency_context_band(record, analysis_result))

    matrix = {name: outcomes[name] for name in outcome_ids}
    matrix["total"] = len(comparisons)
    _require(document.get("confusion_matrix") == matrix,
             f"temporal validation matrix differs for {classifier_id}")
    for name, values in outcome_ids.items():
        _require(document.get(f"{name}_ids") == values,
                 f"temporal {name} list is inconsistent for {classifier_id}")

    admissions = matrix["true_admit"] + matrix["false_admit"]
    rejections = matrix["true_reject"] + matrix["false_reject"] + matrix["abstain"]
    positives = matrix["true_admit"] + matrix["false_reject"]
    definite_negatives = ground_truth_counts["DEFINITE_NEGATIVE"]
    indeterminate = ground_truth_counts["INDETERMINATE"]
    negatives_or_uncertain = definite_negatives + indeterminate
    scored = matrix["total"] - matrix["abstain"]
    denominators = document.get("denominators")
    _require(isinstance(denominators, dict)
             and denominators.get("classifier_admissions") == admissions
             and denominators.get("classifier_rejections") == rejections
             and denominators.get("observer_visual_positives") == positives
             and denominators.get("observer_definite_negatives") == definite_negatives
             and denominators.get("observer_indeterminate") == indeterminate
             and denominators.get("observer_visual_negatives_or_uncertain") ==
                 negatives_or_uncertain
             and denominators.get("scored_observations") == scored
             and denominators.get("false_admit") == {
                 "count": matrix["false_admit"],
                 "denominator_classifier_admissions": admissions,
                 "rate": matrix["false_admit"] / admissions if admissions else None}
             and denominators.get("false_reject") == {
                 "count": matrix["false_reject"],
                 "denominator_observer_visual_positives": positives,
                 "rate": matrix["false_reject"] / positives if positives else None},
             f"temporal denominators were not independently derived for {classifier_id}")
    rates = {
        "accuracy": ((matrix["true_admit"] + matrix["true_reject"]) / scored
                     if scored else None),
        "precision": matrix["true_admit"] / admissions if admissions else None,
        "recall": matrix["true_admit"] / positives if positives else None,
        "specificity": (matrix["true_reject"] / definite_negatives
                        if definite_negatives else None),
    }
    _require(document.get("rates") == rates,
             f"temporal rates were not independently derived for {classifier_id}")
    minima = document.get("numerical_minima")
    _require(isinstance(minima, dict)
             and minima.get("required_true_admit_minimum") == MINIMUM_TEMPORAL_POSITIVES
             and minima.get("required_true_reject_minimum") == MINIMUM_TEMPORAL_NEGATIVES
             and minima.get("observed_true_admit") == matrix["true_admit"]
             and minima.get("observed_true_reject") == matrix["true_reject"]
             and minima.get("true_admit_minimum_met") is
                 (matrix["true_admit"] >= MINIMUM_TEMPORAL_POSITIVES)
             and minima.get("true_reject_minimum_met") is
                 (matrix["true_reject"] >= MINIMUM_TEMPORAL_NEGATIVES),
             f"temporal minima were not independently derived for {classifier_id}")
    branch_allowed = True
    if branch_outcomes:
        branch_matrices = {
            branch: {
                **{name: counts[name] for name in outcome_ids},
                "total": sum(counts.values()),
            }
            for branch, counts in branch_outcomes.items()
        }
        branch_minima = {
            branch: {
                "required_true_admit_minimum": MINIMUM_TEMPORAL_POSITIVES,
                "required_true_reject_minimum": MINIMUM_TEMPORAL_NEGATIVES,
                "required_false_admits": 0,
                "observed_true_admit": values["true_admit"],
                "observed_true_reject": values["true_reject"],
                "observed_false_admit": values["false_admit"],
                "true_admit_minimum_met":
                    values["true_admit"] >= MINIMUM_TEMPORAL_POSITIVES,
                "true_reject_minimum_met":
                    values["true_reject"] >= MINIMUM_TEMPORAL_NEGATIVES,
                "false_admit_requirement_met": values["false_admit"] == 0,
            }
            for branch, values in branch_matrices.items()
        }
        required_bands = spec_validation["required_band_coverage"]
        band_coverage = [band for band in required_bands if band in true_admit_bands]
        branch_allowed = (
            all(values["false_admit"] == 0
                and values["true_admit"] >= MINIMUM_TEMPORAL_POSITIVES
                and values["true_reject"] >= MINIMUM_TEMPORAL_NEGATIVES
                for values in branch_matrices.values())
            and band_coverage == required_bands)
        _require(document.get("branch_confusion_matrices") == branch_matrices
                 and document.get("branch_numerical_minima") == branch_minima
                 and document.get("true_admit_band_coverage") == band_coverage
                 and document.get("required_band_coverage_met") is
                     (band_coverage == required_bands),
                 f"temporal frequency-context strata were not independently derived for {classifier_id}")
    coverage_allowed = True
    if classifier_id == "v1-secondary-text-optical-bridge-v1":
        required_bands = spec_validation["required_band_coverage"]
        band_coverage = [band for band in required_bands if band in true_admit_bands]
        coverage_allowed = band_coverage == required_bands
        _require(document.get("true_admit_band_coverage") == band_coverage
                 and document.get("required_band_coverage_met") is coverage_allowed,
                 "temporal secondary optical band coverage was not independently derived")
    allowed = (matrix["false_admit"] == 0
               and matrix["true_admit"] >= MINIMUM_TEMPORAL_POSITIVES
               and matrix["true_reject"] >= MINIMUM_TEMPORAL_NEGATIVES
               and branch_allowed and coverage_allowed)
    _require(document.get("integrity_pass") is True and allowed,
             f"temporal validation integrity failed for {classifier_id}")
    decision = document.get("allowlist_decision")
    _require(isinstance(decision, dict)
             and decision.get("allowlist_exact_classifier") is allowed,
             f"blind validation did not allow {classifier_id}")
    _require(matrix["false_admit"] == 0,
             f"temporal validation contains a false admission for {classifier_id}")
    _require(matrix["true_admit"] >= MINIMUM_TEMPORAL_POSITIVES,
             f"too few blind positive transitions for {classifier_id}")
    _require(matrix["true_reject"] >= MINIMUM_TEMPORAL_NEGATIVES,
             f"too few blind negative transitions for {classifier_id}")
    sentinels = document.get("mandatory_named_sentinel_audit")
    if sentinels is not None:
        _require(isinstance(sentinels, dict) and sentinels.get("violation_count") == 0,
                 f"mandatory temporal sentinel failed for {classifier_id}")
    return deepcopy(matrix)


def _validate_temporal(document: Any, classifier_id: str, spec_sha256: str,
                       evidence_root: Path, entry: dict[str, Any],
                       implementation: dict[str, str],
                       reader: dict[str, Any], bench_source_sha256: str | None,
                       spec_document: Any) -> dict[str, Any]:
    if isinstance(document, dict) and document.get("schema_version") == 2:
        return _validate_temporal_v2(
            document, classifier_id, spec_sha256, evidence_root, entry,
            implementation, reader, bench_source_sha256, spec_document)
    _require(isinstance(document, dict) and document.get("schema_version") == 1,
             f"invalid temporal validation for {classifier_id}")
    _require(classifier_id == "v1-arrow-phase-edge-v2",
             f"temporal schema version 1 is unsupported for {classifier_id}")
    _require(document.get("classifier_id") == classifier_id
             and document.get("classifier_spec_sha256") == spec_sha256,
             f"temporal validation identity differs for {classifier_id}")

    references = entry.get("source_artifacts")
    _require(isinstance(references, dict) and set(references) == set(TEMPORAL_SOURCE_NAMES),
             f"temporal source artifacts are incomplete for {classifier_id}")
    declared_hashes = document.get("source_artifacts")
    _require(isinstance(declared_hashes, dict),
             f"temporal validation has no source provenance for {classifier_id}")
    source_paths, sources = {}, {}
    for name in TEMPORAL_SOURCE_NAMES:
        field = TEMPORAL_SOURCE_HASH_FIELDS[name]
        _require(references[name].get("sha256") == declared_hashes.get(field),
                 f"temporal source hash differs for {classifier_id}: {name}")
        if name == "observer_readme":
            source_paths[name] = _evidence_file(
                evidence_root, references[name], f"{classifier_id} {name}")
        else:
            source_paths[name], sources[name] = _evidence(
                evidence_root, references[name], f"{classifier_id} {name}")

    seal = sources["seal"]
    for name in ("pre_pixel_freeze", "frozen_classifier_result", "observer_manifest",
                 "observer_readme", "restricted_hidden_key", "selection"):
        field = TEMPORAL_SOURCE_HASH_FIELDS[name]
        _require(seal.get(field) == references[name].get("sha256"),
                 f"temporal seal differs for {classifier_id}: {name}")
    _require(seal.get("classifier_spec_sha256") == spec_sha256
             and seal.get("classifier_source_sha256") ==
                 implementation.get("encounter_arrow_transition.py")
             and seal.get("reader_source_sha256") == implementation.get("encounter_reader.py")
             and seal.get("allowlist_status") == "NOT_ALLOWLISTED",
             f"temporal pre-adjudication seal is invalid for {classifier_id}")
    pre_pixel = sources["pre_pixel_freeze"]
    hidden = sources["restricted_hidden_key"]
    frozen = sources["frozen_classifier_result"]
    _require(pre_pixel.get("allowlist_status") == "NOT_ALLOWLISTED"
             and pre_pixel.get("classifier", {}).get("id") == classifier_id
             and pre_pixel.get("classifier", {}).get("spec_sha256") == spec_sha256,
             f"temporal pre-pixel freeze differs for {classifier_id}")
    _require(hidden.get("do_not_provide_to_observer") is True
             and hidden.get("classifier_id") == classifier_id
             and hidden.get("classifier_spec_sha256") == spec_sha256
             and hidden.get("allowlist_status") == "NOT_ALLOWLISTED_PENDING_BLIND_ADJUDICATION",
             f"temporal hidden key differs for {classifier_id}")
    _require(frozen.get("classifier_id") == classifier_id
             and frozen.get("classifier_spec_sha256") == spec_sha256
             and frozen.get("classifier_source_sha256") ==
                 implementation.get("encounter_arrow_transition.py")
             and frozen.get("reader_source_sha256") == implementation.get("encounter_reader.py")
             and frozen.get("pre_pixel_freeze_sha256") == references["pre_pixel_freeze"].get("sha256")
             and frozen.get("selection_sha256") == references["selection"].get("sha256")
             and frozen.get("errors") == [],
             f"temporal frozen classifier result differs for {classifier_id}")

    observer_manifest = sources["observer_manifest"]
    observations = sources["completed_observations"]
    manifest_items = observer_manifest.get("items")
    observation_items = observations.get("observations")
    hidden_items = hidden.get("items")
    comparisons = document.get("comparisons")
    _require(all(isinstance(value, list) for value in
                 (manifest_items, observation_items, hidden_items, comparisons)),
             f"temporal blind records are malformed for {classifier_id}")
    ids = [[item.get("opaque_id") for item in values if isinstance(item, dict)]
           for values in (manifest_items, observation_items, hidden_items, comparisons)]
    _require(all(len(values) == len(set(values)) for values in ids)
             and ids[0] == ids[1] == ids[2] == ids[3]
             and observer_manifest.get("item_count") == len(ids[0]),
             f"temporal blind record identities differ for {classifier_id}")
    admitted_records = [item.get("frozen_classifier_record") for item in hidden_items
                        if item.get("frozen_classifier_decision") == "ADMITTED"]
    rejected_records = [item.get("frozen_classifier_record") for item in hidden_items
                        if item.get("frozen_classifier_decision") == "REJECTED"]
    canonical = lambda values: Counter(json.dumps(value, sort_keys=True, separators=(",", ":"))
                                       for value in values)
    _require(canonical(admitted_records) == canonical(frozen.get("classifications", []))
             and canonical(rejected_records) == canonical(frozen.get("rejected_runs", [])),
             f"temporal hidden decisions differ from frozen classifier output for {classifier_id}")
    _require(observations.get("instructions_sha256") ==
             references["observer_readme"].get("sha256"),
             f"temporal blind instructions differ for {classifier_id}")

    integrity = document.get("integrity")
    checks = integrity.get("checks") if isinstance(integrity, dict) else None
    clip_checks = integrity.get("clip_checks") if isinstance(integrity, dict) else None
    _require(isinstance(checks, dict) and checks and all(value is True for value in checks.values())
             and isinstance(clip_checks, list),
             f"temporal integrity record failed for {classifier_id}")
    clip_by_id = {item.get("opaque_id"): item for item in clip_checks if isinstance(item, dict)}
    _require(len(clip_by_id) == len(clip_checks) and set(clip_by_id) == set(ids[0]),
             f"temporal clip audit identities differ for {classifier_id}")

    outcomes = Counter()
    outcome_ids = {name: [] for name in
                   ("true_admit", "false_admit", "true_reject", "false_reject")}
    literal_fields = ("center_class", "endpoint_support", "extra_direction_motion", "confidence")
    for manifest_item, observation, hidden_item, comparison in zip(
            manifest_items, observation_items, hidden_items, comparisons):
        opaque_id = comparison["opaque_id"]
        manifest_sha = _digest(manifest_item.get("sha256"), f"temporal clip {opaque_id}")
        _require(hidden_item.get("clip_sha256") == manifest_sha,
                 f"temporal hidden clip hash differs for {opaque_id}")
        clip_path = _evidence_file(source_paths["observer_manifest"].parent,
                                   {"path": manifest_item.get("clip"), "sha256": manifest_sha},
                                   f"temporal clip {opaque_id}")
        _require(type(manifest_item.get("size_bytes")) is int
                 and clip_path.stat().st_size == manifest_item["size_bytes"],
                 f"temporal clip size differs for {opaque_id}")
        clip_audit = clip_by_id[opaque_id]
        _require(clip_audit.get("actual_sha256") == manifest_sha
                 and clip_audit.get("hidden_key_sha256") == manifest_sha
                 and clip_audit.get("sealed_sha256") == manifest_sha
                 and clip_audit.get("actual_size_bytes") == manifest_item["size_bytes"]
                 and clip_audit.get("sealed_size_bytes") == manifest_item["size_bytes"]
                 and all(clip_audit.get(name) is True for name in
                         ("path_matches_id", "source_frame_count_matches_range",
                          "target_run_inside_clip")),
                 f"temporal clip audit differs for {opaque_id}")
        literal = {name: observation.get(name) for name in literal_fields}
        eligible = (literal == {"center_class": "COHERENT_SINGLE_DIRECTION_ON_OFF_EDGE",
                                "endpoint_support": "BOTH_CLEAR",
                                "extra_direction_motion": "NO", "confidence": "HIGH"})
        decision = hidden_item.get("frozen_classifier_decision")
        _require(decision in ("ADMITTED", "REJECTED"),
                 f"temporal frozen decision is invalid for {opaque_id}")
        record = hidden_item.get("frozen_classifier_record")
        _require(isinstance(record, dict), f"temporal frozen record is missing for {opaque_id}")
        target_range = hidden_item.get("target_ambiguous_run_video_indices")
        _require(isinstance(target_range, list) and len(target_range) == 2
                 and all(type(value) is int for value in target_range)
                 and target_range[0] <= target_range[1],
                 f"temporal target range is invalid for {opaque_id}")
        source_indices = list(range(target_range[0], target_range[1] + 1))
        outcome = (("TRUE_ADMIT" if eligible else "FALSE_ADMIT") if decision == "ADMITTED"
                   else ("FALSE_REJECT" if eligible else "TRUE_REJECT"))
        key = outcome.casefold()
        _require(comparison.get("observer_literal") == literal
                 and comparison.get("observer_strict_visual_eligible_for_admission") is eligible
                 and comparison.get("frozen_classifier_decision") == decision
                 and comparison.get("frozen_rejection_code") ==
                 (None if decision == "ADMITTED" else record.get("code"))
                 and comparison.get("endpoint_separation_rms") ==
                 record.get("endpoint_separation_rms")
                 and comparison.get("source_video_frame_indices") == source_indices
                 and comparison.get("comparison_outcome") == outcome,
                 f"temporal comparison was not independently derived for {opaque_id}")
        outcomes[key] += 1
        outcome_ids[key].append(opaque_id)

    matrix = {name: outcomes[name] for name in outcome_ids}
    matrix["total"] = len(comparisons)
    _require(document.get("confusion_matrix") == matrix,
             f"temporal validation matrix differs for {classifier_id}")
    for name, values in outcome_ids.items():
        _require(document.get(f"{name}_ids") == values,
                 f"temporal {name} list is inconsistent for {classifier_id}")

    admissions = matrix["true_admit"] + matrix["false_admit"]
    rejections = matrix["true_reject"] + matrix["false_reject"]
    positives = matrix["true_admit"] + matrix["false_reject"]
    negatives = matrix["true_reject"] + matrix["false_admit"]
    denominators = document.get("denominators")
    _require(isinstance(denominators, dict)
             and denominators.get("classifier_admissions") == admissions
             and denominators.get("classifier_rejections") == rejections
             and denominators.get("observer_visual_positives") == positives
             and denominators.get("observer_visual_negatives_or_uncertain") == negatives
             and denominators.get("false_admit") == {
                 "count": matrix["false_admit"],
                 "denominator_classifier_admissions": admissions,
                 "rate": matrix["false_admit"] / admissions if admissions else None}
             and denominators.get("false_reject") == {
                 "count": matrix["false_reject"],
                 "denominator_observer_visual_positives": positives,
                 "rate": matrix["false_reject"] / positives if positives else None},
             f"temporal denominators were not independently derived for {classifier_id}")
    rates = {"accuracy": (matrix["true_admit"] + matrix["true_reject"]) / matrix["total"],
             "precision": matrix["true_admit"] / admissions if admissions else None,
             "recall": matrix["true_admit"] / positives if positives else None,
             "specificity": matrix["true_reject"] / negatives if negatives else None}
    _require(document.get("rates") == rates,
             f"temporal rates were not independently derived for {classifier_id}")
    minima = document.get("numerical_minima")
    _require(isinstance(minima, dict)
             and minima.get("required_true_admit_minimum") == MINIMUM_TEMPORAL_POSITIVES
             and minima.get("required_true_reject_minimum") == MINIMUM_TEMPORAL_NEGATIVES
             and minima.get("observed_true_admit") == matrix["true_admit"]
             and minima.get("observed_true_reject") == matrix["true_reject"]
             and minima.get("true_admit_minimum_met") is
                 (matrix["true_admit"] >= MINIMUM_TEMPORAL_POSITIVES)
             and minima.get("true_reject_minimum_met") is
                 (matrix["true_reject"] >= MINIMUM_TEMPORAL_NEGATIVES),
             f"temporal minima were not independently derived for {classifier_id}")
    allowed = (matrix["false_admit"] == 0
               and matrix["true_admit"] >= MINIMUM_TEMPORAL_POSITIVES
               and matrix["true_reject"] >= MINIMUM_TEMPORAL_NEGATIVES)
    _require(document.get("integrity_pass") is True and allowed,
             f"temporal validation integrity failed for {classifier_id}")
    decision = document.get("allowlist_decision")
    _require(isinstance(decision, dict)
             and decision.get("allowlist_exact_classifier") is allowed,
             f"blind validation did not allow {classifier_id}")
    _require(matrix["false_admit"] == 0,
             f"temporal validation contains a false admission for {classifier_id}")
    _require(matrix["true_admit"] >= MINIMUM_TEMPORAL_POSITIVES,
             f"too few blind positive transitions for {classifier_id}")
    _require(matrix["true_reject"] >= MINIMUM_TEMPORAL_NEGATIVES,
             f"too few blind negative transitions for {classifier_id}")
    sentinels = document.get("mandatory_named_sentinel_audit")
    if sentinels is not None:
        _require(isinstance(sentinels, dict) and sentinels.get("violation_count") == 0,
                 f"mandatory temporal sentinel failed for {classifier_id}")
    return deepcopy(matrix)


def verify_qualification(path: Path | None, *, implementation_sha256: dict[str, str],
                         reader_runtime: dict[str, Any], camera_name: str,
                         camera_profile: dict[str, Any], policy: dict[str, Any],
                         bench_source_sha256: str | None = None) -> dict[str, Any]:
    """Verify one exact qualification bundle and return a stable fail-closed result."""
    result: dict[str, Any] = {"schema_version": SCHEMA_VERSION,
                              "kind": "encounter_reader_qualification_verification",
                              "status": "REJECTED", "errors": []}
    try:
        _require(path is not None, "reader qualification was not supplied")
        manifest_path = Path(path).resolve()
        manifest = _read_json(manifest_path)
        _require(isinstance(manifest, dict) and manifest.get("schema_version") == SCHEMA_VERSION
                 and manifest.get("kind") == "encounter_reader_qualification",
                 "unsupported reader qualification manifest")
        qualification_id = manifest.get("qualification_id")
        _require(isinstance(qualification_id, str) and qualification_id,
                 "reader qualification has no stable id")
        reader = manifest.get("reader")
        _require(isinstance(reader, dict) and isinstance(reader.get("implementation_sha256"), dict)
                 and isinstance(reader.get("runtime"), dict),
                 "reader qualification identity is malformed")
        implementation = reader["implementation_sha256"]
        _require(set(implementation) == set(STATIC_READER_IMPLEMENTATION_FILES),
                 "reader qualification does not bind the exact static implementation inventory")
        for name, digest in implementation.items():
            _digest(digest, f"reader implementation {name}")
            _require(implementation_sha256.get(name) == digest,
                     f"running implementation differs: {name}")
        _require(reader["runtime"] == reader_runtime,
                 "running reader runtime differs from qualification")
        _require(reader.get("method_version") == reader_runtime.get("method_version"),
                 "reader method version differs from qualification")
        runtime_probe = reader_runtime.get("ocr_runtime_probe")
        _require(reader_runtime.get("ocr_available") is True
                 and reader_runtime.get("ocr_compiled") is True
                 and isinstance(runtime_probe, dict)
                 and runtime_probe.get("status") == "operational",
                 "reader OCR is not operational")
        from encounter_runtime_probe import probe_image_sha256
        try:
            probe_digest = probe_image_sha256()
        except (OSError, ValueError) as exc:
            raise QualificationError("reader OCR probe source is unavailable or invalid") from exc
        _require(runtime_probe.get("probe_sha256") == probe_digest,
                 "reader OCR probe identity differs")
        camera = manifest.get("camera")
        expected_camera = {"name": camera_name, "profile": deepcopy(camera_profile)}
        _require(camera == expected_camera, "camera profile differs from qualification")

        field_path, field_document = _evidence(manifest_path.parent,
                                               manifest.get("field_validation"), "field validation")
        field_summary = _validate_field_evidence(field_document, reader["runtime"], camera,
                                                 implementation, field_path.parent)
        secondary_path, secondary_document = _evidence(
            manifest_path.parent, manifest.get("visible_secondary_validation"),
            "visible-secondary validation")
        secondary_summary = _validate_visible_secondary_evidence(
            secondary_document, reader["runtime"], camera, implementation,
            secondary_path.parent)
        controls_path, controls_document = _evidence(
            manifest_path.parent, manifest.get("fault_controls"), "fault controls")
        controls_summary = _validate_fault_evidence(
            controls_document, reader["runtime"], camera, implementation,
            controls_path.parent)

        qualified_specs = policy.get("qualified_temporal_classifiers")
        _require(isinstance(qualified_specs, dict), "visible-event policy classifiers are malformed")
        contract_version = policy.get("contract_version")
        _require(contract_version in {1, 2, 3},
                 "visible-event policy contract version is malformed")
        temporal = manifest.get("temporal_classifiers")
        _require(isinstance(temporal, dict) and set(temporal) == set(qualified_specs),
                 "qualification temporal classifiers differ from the visible-event policy")
        temporal_summary = {}
        for classifier_id, policy_spec in qualified_specs.items():
            _require(classifier_id in CLASSIFIER_IMPLEMENTATION_FILES,
                     f"unsupported temporal classifier: {classifier_id}")
            _require(isinstance(policy_spec, dict),
                     f"malformed temporal policy: {classifier_id}")
            entry = temporal[classifier_id]
            _require(isinstance(entry, dict), f"malformed temporal qualification: {classifier_id}")
            spec_sha = _digest(policy_spec.get("classifier_spec_sha256"), classifier_id)
            _require(entry.get("classifier_spec_sha256") == spec_sha,
                     f"temporal specification differs: {classifier_id}")
            classifier_implementation = entry.get("implementation_sha256")
            expected_implementation = set(CLASSIFIER_IMPLEMENTATION_FILES[classifier_id])
            _require(isinstance(classifier_implementation, dict)
                     and set(classifier_implementation) == expected_implementation,
                     f"temporal implementation inventory differs: {classifier_id}")
            for name, digest in classifier_implementation.items():
                _digest(digest, f"{classifier_id} implementation {name}")
                _require(implementation_sha256.get(name) == digest,
                         f"running temporal implementation differs: {name}")
            bound_implementation = {**implementation, **classifier_implementation}
            spec_path, spec_document = _evidence(
                manifest_path.parent, entry.get("spec"), f"{classifier_id} specification")
            _require(_sha256(spec_path) == spec_sha,
                     f"temporal specification hash differs: {classifier_id}")
            raw_fields = (TEMPORAL_V2_OBSERVER_RUBRICS[classifier_id]["raw_affected_fields"]
                          if classifier_id in TEMPORAL_V2_OBSERVER_RUBRICS
                          else ["main_arrows"])
            expected_policy_spec = {
                "classifier_spec_sha256": spec_sha,
                "raw_affected_fields": raw_fields,
            }
            if contract_version >= 2:
                semantics = (spec_document.get("deadline_observation_semantics")
                             if isinstance(spec_document, dict) else None)
                _require(semantics in {
                             "LEGAL_PRESENTATION_TRANSITION",
                             "TARGET_ACQUISITION_TRANSITION"},
                         f"temporal specification semantics are invalid: {classifier_id}")
                expected_policy_spec["deadline_observation_semantics"] = semantics
            closure_semantics = (spec_document.get("verification_closure_semantics")
                                 if isinstance(spec_document, dict) else None)
            if closure_semantics is not None:
                _require(closure_semantics ==
                         "RAW_CURRENT_BRACKETED_UNRESOLVED_VERIFICATION_BOUNDARY",
                         f"temporal verification closure semantics are invalid: {classifier_id}")
                expected_policy_spec["verification_closure_semantics"] = closure_semantics
            if "auxiliary_closure_context_ns" in spec_document:
                _require(spec_document["auxiliary_closure_context_ns"] == 80_000_000
                         and classifier_id == "v1-secondary-closed-context-v3",
                         f"temporal auxiliary closure contract is invalid: {classifier_id}")
                expected_policy_spec["auxiliary_closure_context_ns"] = 80_000_000
            _require(policy_spec == expected_policy_spec,
                     f"temporal policy contract differs from qualification: {classifier_id}")
            comparison_path, comparison = _evidence(
                manifest_path.parent, entry.get("validation"),
                f"{classifier_id} validation")
            temporal_summary[classifier_id] = _validate_temporal(
                comparison, classifier_id, spec_sha, comparison_path.parent, entry,
                bound_implementation, reader["runtime"], bench_source_sha256, spec_document)

        result.update(status="QUALIFIED", qualification_id=qualification_id,
                      manifest_sha256=_sha256(manifest_path), manifest_path=str(manifest_path),
                      field_validation={"path": str(field_path), **field_summary},
                      visible_secondary_validation={"path": str(secondary_path),
                                                    **secondary_summary},
                      fault_controls={"path": str(controls_path), **controls_summary},
                      temporal_classifiers=temporal_summary,
                      camera=expected_camera, reader={"method_version": reader["method_version"],
                                                      "implementation_sha256": deepcopy(implementation),
                                                      "runtime": deepcopy(reader_runtime)})
    except (QualificationError, KeyError, TypeError) as exc:
        result["errors"] = [str(exc)]
    return result
