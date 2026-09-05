#!/usr/bin/env python3
"""Focused tests for closed same-frequency context candidates."""
from __future__ import annotations

from copy import deepcopy
import hashlib
import json
from pathlib import Path
import sys
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts" / "bench"))

import encounter_frequency_context as frequency


FIELDS = ("counter_glyph", "primary_frequency", "active_bands", "main_arrows",
          "main_bars", "secondary", "muted_badge")
MASKS = ["abcdg", "bcfg", "abc", "abcdef", "abcdef"]


def target(value="34.700"):
    return {
        "fields": {
            "counter_glyph": {"allowed": ["1"]},
            "primary_frequency": {"allowed": [value]},
            "active_bands": {"allowed": [["Ka"]]},
            "main_arrows": {"allowed": [["front"]]},
            "main_bars": {"allowed": [6]},
            "secondary": {"allowed": [[]]},
            "muted_badge": {"allowed": [False]},
        },
        "joint_states": [{"counter_glyph": "1", "active_bands": ["Ka"],
                          "main_arrows": ["front"]}],
    }


def event(value="34.700"):
    return {"event_id": "event-1", "mode": "UNCHANGED", "changed_fields": [],
            "start_ns": 1, "end_ns": 1_000_000_000, "target": target(value)}


def context(**changes):
    value = {
        "capture_id": "a" * 64,
        "selection_manifest_sha256": "b" * 64,
        "verified_maximum_source_interval_ns": 5_000_000,
        "reader_method_version": frequency.PROFILE_READER_METHOD_VERSION,
        "reader_sha256": frequency.PROFILE_READER_SHA256,
        "redraw_probe_method_version": frequency.PROFILE_REDRAW_PROBE_METHOD_VERSION,
        "redraw_probe_sha256": frequency.PROFILE_REDRAW_PROBE_SHA256,
    }
    value.update(changes)
    return value


def comparison(*, refusal=False, joint="MATCH", other_status=None):
    checks = {field: {"status": "MATCH"} for field in FIELDS}
    if refusal:
        checks["primary_frequency"] = {"status": "UNRESOLVED"}
    if other_status is not None:
        checks["main_bars"] = {"status": other_status}
    value = {"checks": checks, "joint_state": {"status": joint}}
    if joint == "UNRESOLVED":
        value["joint_state"]["reason"] = "joint fields are not all readable/resolved"
    return value


def detail(state, *, off=10.0, partial_p10=36.0):
    if state == "on":
        return {"state": "on", "p10": 80.0, "median": 90.0, "p90": 100.0}
    if state == "off":
        return {"state": "off", "p10": off, "median": off, "p90": off}
    return {"state": "partial", "p10": partial_p10,
            "median": max(partial_p10, 40.0), "p90": 50.0}


def geometry(partials=(), *, off=10.0, partial_p10=36.0, decimal="on", hole=0.0,
             fully_off=(), unexpected_partial=()):
    partials, fully_off, unexpected_partial = set(partials), set(fully_off), set(unexpected_partial)
    raw_cells, profile_cells = [], []
    for digit_index, (origin, mask) in enumerate(zip(frequency._FREQUENCY_ORIGINS, MASKS)):
        segments = {}
        for name in frequency._SEGMENTS:
            key = (digit_index, name)
            expected_on = name in mask
            state = "on" if expected_on else "off"
            if key in partials or key in unexpected_partial:
                state = "partial"
            if key in fully_off:
                state = "off"
            segments[name] = detail(state, off=off, partial_p10=partial_p10)
        raw_mask = "".join(name for name in frequency._SEGMENTS
                           if segments[name]["state"] == "on")
        raw_cells.append({"mask": raw_mask, "segments": deepcopy(segments)})
        profile_cells.append({"origin": origin, "segments": deepcopy(segments),
                              "hole_ink_fractions": [hole, hole]})
    decimal_detail = detail(decimal, off=off)
    return raw_cells, {
        "schema_version": 1,
        "method_version": 1,
        "primary_frequency": {"cells": profile_cells, "decimal": decimal_detail},
    }


def sample(index, *, readable=True, value="34.700", reason=None, partials=(),
           off=10.0, partial_p10=36.0, decimal="on", hole=0.0, fully_off=(),
           unexpected_partial=(), joint="MATCH", other_status=None, capture_ns=None):
    raw_cells, profile = geometry(
        partials, off=off, partial_p10=partial_p10, decimal=decimal, hole=hole,
        fully_off=fully_off, unexpected_partial=unexpected_partial)
    reading = ({"state": "readable", "value": value, "reason": None, "cells": raw_cells}
               if readable else
               {"state": "ambiguous", "value": None, "reason": reason, "cells": raw_cells})
    return {
        "frame_id": f"f{index}",
        "video_frame_index": index,
        "source_frame_seq": index + 100,
        "capture_ns": capture_ns if capture_ns is not None else 10_000_000 + index * 5_000_000,
        "offset_seconds": index / 200,
        "image": f"frames/{index:06d}.png",
        "image_sha256": "c" * 64,
        "observed": {"fields": {"primary_frequency": reading}, "redraw_profiles": profile},
        "expected": target(value),
        "comparison": comparison(refusal=not readable, joint=joint, other_status=other_status),
    }


def chain(*, reason=frequency._BRANCH_REASON[frequency.BRANCH_INTACT_MASK],
          run_length=1, partials=(),
          gap_ns=5_000_000):
    values = []
    for offset in range(run_length + 4):
        readable = offset < 2 or offset >= run_length + 2
        values.append(sample(
            10 + offset, readable=readable, reason=None if readable else reason,
            partials=() if readable else partials,
            capture_ns=10_000_000 + offset * gap_ns))
    return values


class FrequencyContextTests(unittest.TestCase):
    def classify(self, samples, current_event=None, current_context=None):
        return frequency.classify_frequency_context_runs(
            samples, [current_event or event()], current_context or context())

    def test_frozen_intact_only_spec_preserves_reader_and_support_thresholds(self):
        spec_path = (ROOT / "scripts" / "bench" / "temporal_specs" /
                     f"{frequency.CLASSIFIER_ID}.json")
        self.assertEqual(frequency.CLASSIFIER_SPEC_SHA256,
                         hashlib.sha256(spec_path.read_bytes()).hexdigest())
        self.assertEqual(frequency.DEADLINE_OBSERVATION_SEMANTICS,
                         "LEGAL_PRESENTATION_TRANSITION")
        self.assertEqual(frequency.VERIFICATION_CLOSURE_SEMANTICS,
                         "RAW_CURRENT_BRACKETED_UNRESOLVED_VERIFICATION_BOUNDARY")
        spec = json.loads(spec_path.read_text())
        previous = spec_path.with_name("v1-stable-frequency-closed-context-v3.json")
        self.assertEqual(hashlib.sha256(previous.read_bytes()).hexdigest(),
                         "2870acc076f13c186535a47807f9bb55695c929c14599846a4b92034cf50006c")
        old_constants = json.loads(previous.read_text())["constants"]
        self.assertTrue(all(value == old_constants[key]
                            for key, value in spec["constants"].items()))
        self.assertEqual(spec["validation"]["branch_gates"], {
            "intact_mask": {"minimum_blind_true_admits": 5,
                            "minimum_blind_true_rejects": 5, "required_false_admits": 0}})
        self.assertEqual(spec["validation"]["required_band_coverage"], ["X", "K", "Ka"])

    def test_intact_candidate_derives_support_value_and_preserves_input(self):
        samples = chain()
        frozen = deepcopy(samples)
        result = self.classify(samples)
        self.assertEqual(result["errors"], [])
        self.assertEqual(len(result["classifications"]), 1, result)
        record = result["classifications"][0]
        self.assertEqual(record["branch"], frequency.BRANCH_INTACT_MASK)
        self.assertEqual(record["support_derived_frequency"], "34.700")
        self.assertEqual(record["support_derived_digit_masks"], MASKS)
        self.assertEqual(record["video_frame_indices"], [12])
        self.assertEqual(record["verification_closure_semantics"],
                         frequency.VERIFICATION_CLOSURE_SEMANTICS)
        self.assertEqual(record["context_frame_indices"], [12])
        self.assertEqual(record["raw_affected_fields"], ["primary_frequency"])
        self.assertNotIn("partial_segment_evidence", record)
        self.assertEqual(samples, frozen)

    def test_partial_runs_never_generate_admissions(self):
        samples = chain(reason="partial or dim frequency segment interiors", partials=((3, "e"),))
        frozen = deepcopy(samples)
        result = self.classify(samples)
        self.assertEqual(result["classifications"], [])
        self.assertEqual(result["errors"], [])
        self.assertEqual(samples, frozen)

    def test_partial_closing_context_cannot_support_an_intact_run(self):
        for partial_reason in ("partial or dim frequency segment interiors",
                               frequency._BRANCH_REASON[frequency.BRANCH_INTACT_MASK]):
            with self.subTest(reason=partial_reason):
                samples = [sample(10), sample(11), sample(
                    12, readable=False, reason=partial_reason, partials=((3, "e"),)), sample(13),
                    sample(14, readable=False,
                           reason=frequency._BRANCH_REASON[frequency.BRANCH_INTACT_MASK]),
                    sample(15), sample(16)]
                result = self.classify(samples)
                self.assertEqual(result["errors"], [])
                self.assertEqual(result["classifications"], [])
                self.assertIn("CONTEXT_GEOMETRY", {r["code"] for r in result["rejected_runs"]})

    def test_intact_scope_rejects_partial_or_corrupted_pixels_even_with_intact_reason(self):
        cases = {
            "partial": {"partials": ((3, "e"),)},
            "fully_off": {"fully_off": ((3, "e"),)},
            "unexpected_partial": {"unexpected_partial": ((0, "f"),)},
            "decimal": {"decimal": "off"},
            "holes": {"hole": 0.11},
        }
        for name, changes in cases.items():
            with self.subTest(name=name):
                samples = chain()
                samples[2] = sample(12, readable=False,
                                    reason=frequency._BRANCH_REASON[frequency.BRANCH_INTACT_MASK],
                                    capture_ns=samples[2]["capture_ns"], **changes)
                result = self.classify(samples)
                self.assertEqual(result["classifications"], [])
                self.assertEqual(result["rejected_runs"][0]["code"], "FREQUENCY_GEOMETRY")

    def test_readable_support_with_partial_geometry_is_rejected(self):
        samples = chain()
        samples[0] = sample(10, partials=((3, "e"),), capture_ns=samples[0]["capture_ns"])
        result = self.classify(samples)
        self.assertEqual(result["classifications"], [])
        self.assertEqual(result["rejected_runs"][0]["code"], "SUPPORT_GEOMETRY")

    def test_support_is_derived_before_current_target_is_consulted(self):
        samples = chain()
        samples[3]["observed"]["fields"]["primary_frequency"]["value"] = "35.500"
        with patch.object(frequency, "_current_target_frequency",
                          side_effect=AssertionError("target consulted before support closed")):
            result = self.classify(samples)
        self.assertEqual(result["errors"], [])
        self.assertEqual(result["classifications"], [])
        self.assertEqual(result["rejected_runs"][0]["code"], "SUPPORT_VALUE")

        mismatch = self.classify(chain(), current_event=event("35.500"))
        self.assertEqual(mismatch["classifications"], [])
        self.assertEqual(mismatch["rejected_runs"][0]["code"], "TARGET_MISMATCH")

        sample_mismatch = chain()
        sample_mismatch[2]["expected"] = target("35.500")
        mismatch = self.classify(sample_mismatch)
        self.assertEqual(mismatch["classifications"], [])
        self.assertEqual(mismatch["rejected_runs"][0]["code"], "TARGET_MISMATCH")

    def test_comparison_and_joint_state_scope_is_exact(self):
        samples = chain()
        samples[2]["comparison"] = comparison(refusal=True, joint="UNRESOLVED")
        accepted = self.classify(samples)
        self.assertEqual(len(accepted["classifications"]), 1, accepted)

        cases = []
        difference = chain()
        difference[2]["comparison"] = comparison(
            refusal=True, joint="UNRESOLVED", other_status="DIFFERENCE")
        cases.append(difference)
        nonmatch = chain()
        nonmatch[2]["comparison"] = comparison(
            refusal=True, joint="MATCH", other_status="UNRESOLVED")
        cases.append(nonmatch)
        bad_joint = chain()
        bad_joint[2]["comparison"] = comparison(refusal=True, joint="DIFFERENCE")
        cases.append(bad_joint)
        for scoped in cases:
            result = self.classify(scoped)
            self.assertEqual(result["classifications"], [])
            self.assertEqual(result["rejected_runs"][0]["code"], "PRODUCT_FIELD_SCOPE")

    def test_source_run_and_support_span_bounds_fail_closed(self):
        source_gap = chain()
        source_gap[2]["source_frame_seq"] += 1
        result = self.classify(source_gap)
        self.assertEqual(result["classifications"], [])
        self.assertEqual(result["rejected_runs"][0]["code"], "SOURCE_GAP")

        long_run = chain(run_length=17)
        result = self.classify(long_run)
        self.assertEqual(result["classifications"], [])
        self.assertEqual(result["rejected_runs"][0]["code"], "RUN_SPAN")

        # Each refusal is only one frame and every intervening readable frame
        # keeps the same value.  With no pair of readable closing supports until
        # the end, the 320 ms episode must still fail the independent context cap.
        support_span = [sample(10, capture_ns=10_000_000),
                        sample(11, capture_ns=20_000_000)]
        intact_reason = frequency._BRANCH_REASON[frequency.BRANCH_INTACT_MASK]
        for offset in range(29):
            index = 12 + offset
            support_span.append(sample(
                index, readable=(offset % 2 == 1),
                reason=None if offset % 2 == 1 else intact_reason,
                capture_ns=30_000_000 + offset * 10_000_000))
        support_span.extend([
            sample(41, capture_ns=320_000_000),
            sample(42, capture_ns=330_000_000),
        ])
        result = self.classify(support_span, current_context=context(
            verified_maximum_source_interval_ns=10_000_000))
        self.assertEqual(result["classifications"], [])
        self.assertTrue(result["rejected_runs"])
        self.assertEqual({item["code"] for item in result["rejected_runs"]},
                         {"SUPPORT_SPAN"})

        unclosed = self.classify(chain()[1:])
        self.assertEqual(unclosed["classifications"], [])
        self.assertEqual(unclosed["rejected_runs"][0]["code"], "UNCLOSED_RUN")

    def test_conflicting_duplicate_and_bad_context_clear_all_candidates(self):
        samples = chain()
        conflict = deepcopy(samples[2])
        conflict["comparison"] = comparison(refusal=True, other_status="DIFFERENCE")
        duplicate = self.classify(samples + [conflict])
        self.assertEqual(duplicate["classifications"], [])
        self.assertIn("conflicting frequency evidence", duplicate["errors"][0])

        malformed = self.classify(samples, current_context=context(reader_method_version=6))
        self.assertEqual(malformed["classifications"], [])
        self.assertIn("does not match the frozen optical profile", malformed["errors"][0])


if __name__ == "__main__":
    unittest.main()
