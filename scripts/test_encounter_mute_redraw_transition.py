#!/usr/bin/env python3
"""Focused tests for mute-on badge and unmute frequency redraw candidates."""
from __future__ import annotations

from copy import deepcopy
import hashlib
import sys
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts" / "bench"))

import encounter_mute_redraw_transition as redraw


FIELDS = ("counter_glyph", "primary_frequency", "active_bands", "main_arrows",
          "main_bars", "secondary", "muted_badge")


def target(muted):
    fields = {
        "counter_glyph": {"allowed": ["1"]},
        "primary_frequency": {"allowed": ["34.700"]},
        "active_bands": {"allowed": [["Ka"]]},
        "main_arrows": {"allowed": [["front"]]},
        "main_bars": {"allowed": [6]},
        "secondary": {"allowed": [[]]},
        "muted_badge": {"allowed": [muted]},
    }
    return {"fields": fields,
            "joint_states": [{"counter_glyph": "1", "active_bands": ["Ka"],
                              "main_arrows": ["front"]}]}


def event(previous, current):
    return {"event_id": "event-1", "mode": "CHANGED", "changed_fields": ["muted_badge"],
            "start_ns": 1, "end_ns": 100_000_000,
            "previous_target": target(previous), "target": target(current)}


def context():
    return {"capture_id": "a" * 64, "selection_manifest_sha256": "b" * 64,
            "verified_maximum_source_interval_ns": 5_000_000,
            "reader_method_version": redraw.PROFILE_READER_METHOD_VERSION,
            "reader_sha256": redraw.PROFILE_READER_SHA256,
            "redraw_probe_method_version": redraw.PROFILE_REDRAW_PROBE_METHOD_VERSION,
            "redraw_probe_sha256": redraw.PROFILE_REDRAW_PROBE_SHA256}


def comparisons(field, unresolved=True, other_difference=None):
    checks = {name: {"status": "MATCH"} for name in FIELDS}
    if unresolved:
        checks[field] = {"status": "UNRESOLVED"}
    if other_difference:
        checks[other_difference] = {"status": "DIFFERENCE"}
    return {"checks": checks, "joint_state": {"status": "MATCH"}}


def base_sample(index, capture_ns):
    return {"frame_id": f"f{index}", "video_frame_index": index,
            "source_frame_seq": index + 100, "capture_ns": capture_ns,
            "image": f"frames/{index:06d}.png", "image_sha256": "c" * 64,
            "observed": {"fields": {}}, "comparison": comparisons("muted_badge", False)}


def badge_profile(value, field_state):
    if field_state == "false":
        fraction, p95 = 0.0, 20.0
    elif field_state == "true":
        fraction, p95 = 0.5, 100.0
    else:
        fraction, p95 = 0.1, 50.0
    return {"schema_version": 1, "method_version": 1,
            "muted_badge": {"box": list(redraw._MUTED_BADGE_BOX),
                            "lit_fraction": fraction, "p95": p95,
                            "profile_schema": {"rows": 2, "columns": 4,
                                               "sample": "max-channel cell median"},
                            "profile": [float(value)] * 8}}


def badge_samples():
    samples = []
    states = [(False, "false", 10), (False, "false", 10),
              (None, "ambiguous", 25), (True, "true", 45), (True, "true", 45)]
    for offset, (value, state, profile) in enumerate(states):
        sample = base_sample(10 + offset, 10_000_000 + offset * 5_000_000)
        if state == "ambiguous":
            reading = {"state": "ambiguous", "value": None, "reason": "partial muted badge"}
            sample["comparison"] = comparisons("muted_badge")
        else:
            reading = {"state": "readable", "value": value, "reason": None}
        sample["observed"]["fields"]["muted_badge"] = reading
        sample["observed"]["redraw_profiles"] = badge_profile(profile, state)
        samples.append(sample)
    return samples


def frequency_probe(level):
    masks = ["abcdg", "bcfg", "abc", "abcdef", "abcdef"]
    cells = []
    for origin, mask in zip(redraw._FREQUENCY_ORIGINS, masks):
        segments = {name: {"state": "on" if name in mask else "off",
                           "p10": level if name in mask else 10.0,
                           "median": level if name in mask else 10.0,
                           "p90": level if name in mask else 10.0}
                    for name in redraw._SEGMENTS}
        cells.append({"origin": origin, "segments": segments,
                      "hole_ink_fractions": [0.0, 0.0]})
    return {"schema_version": 1, "method_version": 1,
            "primary_frequency": {"cells": cells,
                                  "decimal": {"state": "on", "p10": level,
                                              "median": level, "p90": level}}}


def frequency_cells(level):
    masks = ["abcdg", "bcfg", "abc", "abcdef", "abcdef"]
    return [{"mask": mask,
             "segments": {name: {"state": "on" if name in mask else "off",
                                  "p10": level if name in mask else 10.0,
                                  "median": level if name in mask else 10.0,
                                  "p90": level if name in mask else 10.0}
                          for name in redraw._SEGMENTS}}
            for mask in masks]


def frequency_samples():
    samples = []
    levels = [50, 50, 60, 75, 90, 105, 110, 110]
    for offset, level in enumerate(levels):
        sample = base_sample(20 + offset, 10_000_000 + offset * 5_000_000)
        if 2 <= offset <= 5:
            reading = {"state": "ambiguous", "value": None,
                       "reason": "inconsistent illuminated frequency segment levels",
                       "cells": frequency_cells(level)}
            sample["comparison"] = comparisons(
                "primary_frequency", other_difference="muted_badge" if offset == 2 else None)
        else:
            reading = {"state": "readable", "value": "34.700", "reason": None,
                       "cells": frequency_cells(level)}
            sample["comparison"] = comparisons("primary_frequency", False)
        sample["observed"]["fields"]["primary_frequency"] = reading
        sample["observed"]["redraw_profiles"] = frequency_probe(level)
        samples.append(sample)
    return samples


class MuteRedrawTests(unittest.TestCase):
    def test_classifier_and_probe_hashes_match_frozen_sources(self):
        spec_root = ROOT / "scripts" / "bench" / "temporal_specs"
        self.assertEqual(redraw.BADGE_CLASSIFIER_SPEC_SHA256, hashlib.sha256(
            (spec_root / f"{redraw.BADGE_CLASSIFIER_ID}.json").read_bytes()).hexdigest())
        self.assertEqual(redraw.FREQUENCY_CLASSIFIER_SPEC_SHA256, hashlib.sha256(
            (spec_root / f"{redraw.FREQUENCY_CLASSIFIER_ID}.json").read_bytes()).hexdigest())
        self.assertEqual(redraw.PROFILE_REDRAW_PROBE_SHA256, hashlib.sha256(
            (ROOT / "scripts" / "bench" / "encounter_redraw_probe.py").read_bytes()).hexdigest())

    def test_badge_rising_fill_is_candidate_and_input_is_immutable(self):
        samples = badge_samples()
        frozen = deepcopy(samples)
        result = redraw.classify_mute_redraw_runs(samples, [event(False, True)], context())
        self.assertEqual(result["errors"], [])
        self.assertEqual(len(result["classifications"]), 1)
        record = result["classifications"][0]
        self.assertEqual(record["classifier_id"], redraw.BADGE_CLASSIFIER_ID)
        self.assertEqual(record["video_frame_indices"], [12])
        self.assertEqual(record["raw_affected_fields"], ["muted_badge"])
        self.assertEqual(samples, frozen)

    def test_badge_reverse_event_and_backtracking_are_rejected(self):
        result = redraw.classify_mute_redraw_runs(
            badge_samples(), [event(True, False)], context())
        self.assertEqual(result["classifications"], [])
        self.assertEqual(result["rejected_runs"][0]["code"], "EVENT_SCOPE")

        samples = badge_samples()
        samples[2]["observed"]["redraw_profiles"]["muted_badge"]["profile"][0] = 43.0
        samples[3]["observed"]["redraw_profiles"]["muted_badge"]["profile"][0] = 25.0
        result = redraw.classify_mute_redraw_runs(samples, [event(False, True)], context())
        self.assertEqual(result["classifications"], [])
        self.assertIn(result["rejected_runs"][0]["code"],
                      {"MAXIMUM_BACKWARD_STEP", "TOTAL_BACKWARD_MOTION"})

    def test_badge_requires_exact_fixed_measurement_schema(self):
        cases = []
        wrong_box = badge_samples()
        wrong_box[2]["observed"]["redraw_profiles"]["muted_badge"]["box"][0] += 1
        cases.append(wrong_box)
        invalid_fraction = badge_samples()
        invalid_fraction[2]["observed"]["redraw_profiles"]["muted_badge"][
            "lit_fraction"] = float("inf")
        cases.append(invalid_fraction)
        for samples in cases:
            result = redraw.classify_mute_redraw_runs(
                samples, [event(False, True)], context())
            self.assertEqual(result["classifications"], [])
            self.assertEqual(result["rejected_runs"][0]["code"], "PROFILE_SCHEMA")

    def test_frequency_claim_excludes_preexisting_badge_frame(self):
        result = redraw.classify_mute_redraw_runs(
            frequency_samples(), [event(True, False)], context())
        self.assertEqual(result["errors"], [])
        self.assertEqual(len(result["classifications"]), 1)
        record = result["classifications"][0]
        self.assertEqual(record["classifier_id"], redraw.FREQUENCY_CLASSIFIER_ID)
        self.assertEqual(record["full_field_run_indices"], [22, 23, 24, 25])
        self.assertEqual(record["video_frame_indices"], [23, 24, 25])
        self.assertEqual(record["raw_affected_fields"], ["primary_frequency"])

    def test_frequency_requires_clear_holes_and_stable_value(self):
        samples = frequency_samples()
        samples[3]["observed"]["redraw_profiles"]["primary_frequency"]["cells"][0][
            "hole_ink_fractions"][0] = 0.11
        result = redraw.classify_mute_redraw_runs(samples, [event(True, False)], context())
        self.assertEqual(result["classifications"], [])
        self.assertEqual(result["rejected_runs"][0]["code"], "FREQUENCY_PROFILE")

        samples = frequency_samples()
        samples[-1]["observed"]["fields"]["primary_frequency"]["value"] = "35.500"
        result = redraw.classify_mute_redraw_runs(samples, [event(True, False)], context())
        self.assertEqual(result["classifications"], [])
        self.assertEqual(result["rejected_runs"][0]["code"], "STABLE_FREQUENCY_SUPPORT")

        samples = frequency_samples()
        samples[3]["observed"]["redraw_profiles"]["primary_frequency"]["decimal"][
            "p10"] = 44.0
        result = redraw.classify_mute_redraw_runs(samples, [event(True, False)], context())
        self.assertEqual(result["classifications"], [])
        self.assertEqual(result["rejected_runs"][0]["code"], "FREQUENCY_PROFILE")

    def test_context_mismatch_refuses_all_classification(self):
        invalid = context()
        invalid["redraw_probe_method_version"] = 2
        result = redraw.classify_mute_redraw_runs(
            badge_samples(), [event(False, True)], invalid)
        self.assertEqual(result["classifications"], [])
        self.assertRegex(result["errors"][0], "frozen optical profile")


if __name__ == "__main__":
    unittest.main()
