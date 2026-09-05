#!/usr/bin/env python3
"""Focused controls for the closed secondary-card context classifier."""
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

import encounter_secondary_context as secondary


FIELDS = ("counter_glyph", "primary_frequency", "active_bands", "main_arrows",
          "main_bars", "secondary", "muted_badge")


def card_value(*, band="K", frequency="24.150", direction="side", bars=3):
    return {"band": band, "frequency": frequency,
            "direction": direction, "bars": bars}


def target(cards=None):
    cards = [card_value()] if cards is None else deepcopy(cards)
    return {
        "input": {"ready": True},
        "fields": {
            "counter_glyph": {"allowed": ["2"]},
            "primary_frequency": {"allowed": ["34.700"]},
            "active_bands": {"allowed": [["Ka"]]},
            "main_arrows": {"allowed": [["front"]]},
            "main_bars": {"allowed": [5]},
            "secondary": {"allowed": [cards]},
            "muted_badge": {"allowed": [False]},
        },
        "joint_states": [{"counter_glyph": "2", "active_bands": ["Ka"],
                          "main_arrows": ["front"]}],
    }


def event(cards=None):
    return {"event_id": "event-1", "mode": "UNCHANGED", "changed_fields": [],
            "start_ns": 1, "end_ns": 1_000_000_000, "target": target(cards)}


def context(**changes):
    value = {
        "capture_id": "a" * 64,
        "selection_manifest_sha256": "b" * 64,
        "verified_maximum_source_interval_ns": 5_000_000,
        "reader_method_version": secondary.PROFILE_READER_METHOD_VERSION,
        "reader_sha256": secondary.PROFILE_READER_SHA256,
    }
    value.update(changes)
    return value


def comparison(*, refusal=False, other_status=None, joint="MATCH",
               secondary_status=None):
    checks = {field: {"status": "MATCH"} for field in FIELDS}
    if refusal:
        checks["secondary"] = {"status": secondary_status or "UNRESOLVED"}
    if other_status is not None:
        checks["main_bars"] = {"status": other_status}
    return {"checks": checks, "joint_state": {"status": joint}}


def meter(count, partial_cells=()):
    partial_cells = set(partial_cells)
    states = [("partial" if index in partial_cells else
               "on" if index < count else "off") for index in range(6)]
    compatible = secondary._compatible_counts(states)
    bars = [{"state": state, "quadrants": [], "spatial_ink_supported": state != "off",
             "background_contrast": 20.0 if state != "off" else 0.0,
             "interior": [index, 0, index + 1, 1]}
            for index, state in enumerate(states)]
    if partial_cells:
        return {"state": "ambiguous", "value": None,
                "reason": secondary.RAW_PARTIAL_METER_REASON,
                "bars": bars, "compatible_counts": compatible}
    return {"state": "readable", "value": count, "reason": None,
            "bars": bars, "compatible_counts": compatible}


def card_observation(value, slot, *, partial_cells=()):
    bar_reading = meter(value["bars"], partial_cells)
    partial = bool(partial_cells)
    return {
        "slot": slot,
        "band": value["band"],
        "frequency": value["frequency"],
        "direction": value["direction"],
        "bars": None if partial else value["bars"],
        "compatible_bars": deepcopy(bar_reading["compatible_counts"]),
        "text_visible": True,
        "bars_state": bar_reading["state"],
        "bar_reading": bar_reading,
        "direction_reading": {
            "state": "readable", "value": value["direction"], "reason": None,
            "symbol_bounds": [1, 2, 3, 4], "reference_size": [2.0, 2.0],
            "shape_matches": [value["direction"]],
        },
        "ocr_observation": {"revision": 3, "rows": []},
        "ocr_candidates": [[value["band"], value["frequency"]]],
    }


def secondary_reading(values, *, partial_by_slot=None):
    partial_by_slot = partial_by_slot or {}
    cards = [card_observation(value, slot,
                              partial_cells=partial_by_slot.get(slot, ()))
             for slot, value in enumerate(values)]
    if partial_by_slot:
        return {
            "state": "unreadable", "value": None,
            "reason": secondary.RAW_UNREADABLE_REASON,
            "partial_cards": [{key: card.get(key) for key in secondary._CARD_KEYS}
                              for card in cards],
            "cards": cards, "ocr_available": True,
        }
    return {"state": "readable", "value": deepcopy(values), "reason": None,
            "cards": cards}


def sample(index, *, values=None, partial_by_slot=None, capture_ns=None,
           other_status=None, joint="MATCH", secondary_status=None):
    values = [card_value()] if values is None else deepcopy(values)
    refusal = bool(partial_by_slot)
    return {
        "frame_id": f"f{index}",
        "video_frame_index": index,
        "source_frame_seq": index + 100,
        "capture_ns": capture_ns if capture_ns is not None else 10_000_000 + index * 5_000_000,
        "offset_seconds": index / 200,
        "image": f"frames/{index:06d}.png",
        "image_sha256": "c" * 64,
        "observed": {"fields": {"secondary": secondary_reading(
            values, partial_by_slot=partial_by_slot)}},
        "expected": target(values),
        "comparison": comparison(
            refusal=refusal, other_status=other_status, joint=joint,
            secondary_status=secondary_status),
    }


def chain(*, values=None, partial_by_slot=None, run_length=1, gap_ns=5_000_000):
    values = [card_value()] if values is None else deepcopy(values)
    partial_by_slot = {0: (5,)} if partial_by_slot is None else partial_by_slot
    result = []
    for offset in range(run_length + 4):
        refusal = 2 <= offset < run_length + 2
        result.append(sample(
            10 + offset, values=values,
            partial_by_slot=partial_by_slot if refusal else None,
            capture_ns=10_000_000 + offset * gap_ns))
    return result


class SecondaryContextTests(unittest.TestCase):
    def classify(self, samples, *, current_event=None, current_context=None):
        if current_event is None:
            current_event = event()
            current_event["first_correct"] = {
                key: deepcopy(samples[0][key])
                for key in ("frame_id", "video_frame_index", "source_frame_seq",
                            "capture_ns", "offset_seconds", "image")}
        return secondary.classify_secondary_context_runs(
            samples, [current_event], current_context or context())

    def test_frozen_spec_identity_and_semantics(self):
        spec_path = (ROOT / "scripts" / "bench" / "temporal_specs" /
                     f"{secondary.CLASSIFIER_ID}.json")
        self.assertEqual(secondary.CLASSIFIER_SPEC_SHA256,
                         hashlib.sha256(spec_path.read_bytes()).hexdigest())
        self.assertEqual(secondary.DEADLINE_OBSERVATION_SEMANTICS,
                         "LEGAL_PRESENTATION_TRANSITION")
        spec = json.loads(spec_path.read_text())
        self.assertEqual(spec["identity"]["reader_sha256"],
                         secondary.PROFILE_READER_SHA256)
        self.assertEqual(spec["validation"]["required_false_admits"], 0)

    def test_one_card_exact_count_closure_is_recorded_without_mutating_raw_input(self):
        samples = chain()
        frozen = deepcopy(samples)
        result = self.classify(samples)
        self.assertEqual(result["errors"], [])
        self.assertEqual(len(result["classifications"]), 1, result)
        record = result["classifications"][0]
        self.assertEqual(record["video_frame_indices"], [12])
        self.assertEqual(record["raw_affected_fields"], ["secondary"])
        self.assertEqual(record["support_derived_secondary"], [card_value()])
        self.assertEqual(record["resolved_value"], [card_value()])
        self.assertEqual(record["deadline_observation_semantics"],
                         "LEGAL_PRESENTATION_TRANSITION")
        self.assertEqual(record["verification_closure_semantics"],
                         "RAW_CURRENT_BRACKETED_UNRESOLVED_VERIFICATION_BOUNDARY")
        self.assertEqual(record["auxiliary_closure_context_ns"], 80_000_000)
        self.assertEqual(record["context_frame_indices"], record["full_context_indices"])
        self.assertEqual(record["partial_meter_evidence"][0]["cards"][0], {
            "slot": 0, "state": "partial", "bars": None,
            "partial_cells": [5], "compatible_bars": [3],
            "cell_states": ["on", "on", "on", "off", "off", "partial"],
        })
        self.assertEqual(record["compatible_bar_intersections"], [[3]])
        self.assertEqual(record["capture_id"], "a" * 64)
        self.assertEqual(record["selection_manifest_sha256"], "b" * 64)
        self.assertEqual(record["current_presentation_established"]["capture_ns"],
                         samples[0]["capture_ns"])
        self.assertEqual(samples, frozen)

    def test_two_card_context_allows_one_readable_and_one_boundary_partial_meter(self):
        values = [card_value(), card_value(
            band="Ka", frequency="35.500", direction="rear", bars=3)]
        samples = chain(values=values, partial_by_slot={1: (5,)})
        current_event = event(values)
        current_event["first_correct"] = {
            key: deepcopy(samples[0][key])
            for key in ("frame_id", "video_frame_index", "source_frame_seq",
                        "capture_ns", "offset_seconds", "image")}
        result = self.classify(samples, current_event=current_event)
        self.assertEqual(len(result["classifications"]), 1, result)
        cards = result["classifications"][0]["partial_meter_evidence"][0]["cards"]
        self.assertEqual([card["state"] for card in cards], ["readable", "partial"])
        self.assertEqual(cards[1]["compatible_bars"], [3])

    def test_support_is_closed_before_target_is_consulted(self):
        samples = chain()
        different = card_value(band="Ka", frequency="35.500", direction="rear")
        samples[3] = sample(13, values=[different], capture_ns=samples[3]["capture_ns"])
        with patch.object(secondary, "_current_target_secondary",
                          side_effect=AssertionError("target consulted before support closed")):
            result = self.classify(samples)
        self.assertEqual(result["errors"], [])
        self.assertEqual(result["classifications"], [])
        self.assertEqual(result["rejected_runs"][0]["code"], "SUPPORT_VALUE")

        mismatch = self.classify(chain(), current_event=event([
            card_value(band="Ka", frequency="35.500", direction="rear")]))
        self.assertEqual(mismatch["classifications"], [])
        self.assertEqual(mismatch["rejected_runs"][0]["code"], "TARGET_MISMATCH")

    def test_partial_card_must_preserve_identity_and_compatible_meter_evidence(self):
        cases = {}
        incomplete = chain()
        reading = incomplete[2]["observed"]["fields"]["secondary"]
        reading["cards"][0]["band"] = None
        reading["partial_cards"][0]["band"] = None
        cases["incomplete_identity"] = incomplete

        wrong_ocr = chain()
        wrong_ocr[2]["observed"]["fields"]["secondary"]["cards"][0]["ocr_candidates"] = [["X", "10.525"]]
        cases["wrong_ocr"] = wrong_ocr

        incompatible = chain()
        card = incompatible[2]["observed"]["fields"]["secondary"]["cards"][0]
        card["compatible_bars"] = [3, 4]
        cases["inconsistent_compatible_counts"] = incompatible

        no_partial = chain()
        no_partial[2]["observed"]["fields"]["secondary"] = secondary_reading([card_value()])
        no_partial[2]["observed"]["fields"]["secondary"].update(
            state="unreadable", value=None, reason=secondary.RAW_UNREADABLE_REASON,
            partial_cards=[card_value()], ocr_available=True)
        cases["no_partial_meter"] = no_partial

        for name, samples in cases.items():
            with self.subTest(name=name):
                result = self.classify(samples)
                self.assertEqual(result["errors"], [])
                self.assertEqual(result["classifications"], [])
                self.assertEqual(result["rejected_runs"][0]["code"],
                                 "CARD_REDRAW_CONTEXT")

        not_closed = self.classify(chain(partial_by_slot={0: (3, 4)}))
        self.assertEqual(not_closed["classifications"], [])
        self.assertEqual(not_closed["rejected_runs"][0]["code"],
                         "METER_COMPATIBILITY")

    def test_multiple_frame_compatibility_can_close_only_on_the_support_count(self):
        samples = chain(partial_by_slot={0: (3,)}, run_length=2)
        samples[3] = sample(13, partial_by_slot={0: (5,)},
                            capture_ns=samples[3]["capture_ns"])
        result = self.classify(samples)
        self.assertEqual(len(result["classifications"]), 1, result)
        record = result["classifications"][0]
        self.assertEqual(record["compatible_bar_intersections"], [[3]])
        self.assertEqual(
            [frame["cards"][0]["compatible_bars"]
             for frame in record["partial_meter_evidence"]],
            [[3, 4], [3]])

    def test_two_readable_frames_can_bridge_refusal_runs_inside_one_closed_context(self):
        samples = [
            sample(10), sample(11),
            sample(12, partial_by_slot={0: (3,)}),
            sample(13), sample(14),
            sample(15, partial_by_slot={0: (5,)}),
            sample(16), sample(17),
        ]
        result = self.classify(samples)
        self.assertEqual(result["errors"], [])
        self.assertEqual([record["video_frame_indices"]
                          for record in result["classifications"]], [[12], [15]])
        for record in result["classifications"]:
            self.assertEqual(record["full_context_indices"], list(range(10, 18)))
            self.assertEqual(record["context_refusal_indices"], [12, 15])
            self.assertEqual(record["interleaved_readable_indices"], [13, 14])
            self.assertEqual(record["compatible_bar_intersections"], [[3]])

    def test_three_readable_frames_end_context_without_retrying_the_prior_refusal(self):
        samples = [
            sample(10), sample(11),
            sample(12, partial_by_slot={0: (3,)}),
            sample(13), sample(14), sample(15),
            sample(16, partial_by_slot={0: (5,)}),
            sample(17), sample(18),
        ]
        result = self.classify(samples)
        self.assertEqual([record["video_frame_indices"]
                          for record in result["classifications"]], [[16]])
        self.assertEqual(len(result["rejected_runs"]), 1)
        self.assertEqual(result["rejected_runs"][0]["code"],
                         "METER_COMPATIBILITY")

    def test_interleaved_readable_frame_must_preserve_exact_current_card_identity(self):
        samples = [
            sample(10), sample(11),
            sample(12, partial_by_slot={0: (3,)}), sample(13),
            sample(14, partial_by_slot={0: (5,)}),
            sample(15), sample(16),
        ]
        different = card_value(band="Ka", frequency="35.500", direction="rear")
        samples[3]["observed"]["fields"]["secondary"] = secondary_reading([different])
        result = self.classify(samples)
        self.assertEqual(result["classifications"], [])
        self.assertEqual(result["rejected_runs"][0]["code"], "INTERIOR_CONTEXT")

    def test_product_scope_requires_only_secondary_unresolved_and_current(self):
        cases = []
        other_difference = chain()
        other_difference[2]["comparison"] = comparison(
            refusal=True, other_status="DIFFERENCE")
        cases.append(other_difference)
        other_unresolved = chain()
        other_unresolved[2]["comparison"] = comparison(
            refusal=True, other_status="UNRESOLVED")
        transition = chain()
        transition[2]["comparison"] = comparison(
            refusal=True, other_status="TRANSITION_DIFFERENCE")
        cases.append(transition)
        wrong_secondary = chain()
        wrong_secondary[2]["comparison"] = comparison(
            refusal=True, secondary_status="TRANSITION_DIFFERENCE")
        cases.append(wrong_secondary)
        bad_joint = chain()
        bad_joint[2]["comparison"] = comparison(refusal=True, joint="DIFFERENCE")
        cases.append(bad_joint)
        for samples in cases:
            result = self.classify(samples)
            self.assertEqual(result["classifications"], [])
            self.assertEqual(result["rejected_runs"][0]["code"],
                             "PRODUCT_FIELD_SCOPE")

        secondary_only = self.classify(other_unresolved)
        self.assertEqual(len(secondary_only["classifications"]), 1)
        self.assertEqual(
            secondary_only["classifications"][0]["raw_affected_fields"],
            ["secondary"])

        joint_unknown = chain()
        joint_unknown[2]["comparison"] = comparison(refusal=True, joint="UNRESOLVED")
        self.assertEqual(len(self.classify(joint_unknown)["classifications"]), 1)

        support_unknown = chain()
        support_unknown[0]["comparison"] = comparison(other_status="UNRESOLVED",
                                                        joint="UNRESOLVED")
        current_event = event()
        current_event["first_correct"] = {
            key: deepcopy(support_unknown[1][key])
            for key in ("frame_id", "video_frame_index", "source_frame_seq",
                        "capture_ns", "offset_seconds", "image")}
        self.assertEqual(len(self.classify(
            support_unknown, current_event=current_event)["classifications"]), 1)

        support_noncurrent = chain()
        support_noncurrent[0]["comparison"] = comparison(
            other_status="TRANSITION_DIFFERENCE")
        rejected = self.classify(support_noncurrent)
        self.assertEqual(rejected["classifications"], [])
        self.assertEqual(rejected["rejected_runs"][0]["code"],
                         "SUPPORT_COMPARISON")

    def test_expectation_change_source_gap_and_span_fail_closed(self):
        changed = chain()
        changed[2]["expected"] = target([card_value(
            band="Ka", frequency="35.500", direction="rear")])
        result = self.classify(changed)
        self.assertEqual(result["classifications"], [])
        self.assertEqual(result["rejected_runs"][0]["code"], "TARGET_MISMATCH")

        source_gap = chain()
        source_gap[2]["source_frame_seq"] += 1
        result = self.classify(source_gap)
        self.assertEqual(result["classifications"], [])
        self.assertEqual(result["rejected_runs"][0]["code"], "SOURCE_GAP")

        recording_bound = self.classify(
            chain(), current_context=context(verified_maximum_source_interval_ns=3_000_000))
        self.assertEqual(recording_bound["classifications"], [])
        self.assertEqual(recording_bound["rejected_runs"][0]["code"], "SOURCE_GAP")

        long_run = self.classify(chain(run_length=13))
        self.assertEqual(long_run["classifications"], [])
        self.assertEqual(long_run["rejected_runs"][0]["code"], "CONTEXT_SPAN")

        support_span = self.classify(
            chain(run_length=7, gap_ns=10_000_000),
            current_context=context(verified_maximum_source_interval_ns=10_000_000))
        self.assertEqual(support_span["classifications"], [])
        self.assertEqual(support_span["rejected_runs"][0]["code"], "SUPPORT_SPAN")

    def test_refusal_before_first_fully_current_observation_is_not_acquisition(self):
        samples = chain()
        acquiring = event()
        acquiring["first_correct"] = {
            key: deepcopy(samples[3][key])
            for key in ("frame_id", "video_frame_index", "source_frame_seq",
                        "capture_ns", "offset_seconds", "image")}
        result = self.classify(samples, current_event=acquiring)
        self.assertEqual(result["classifications"], [])
        self.assertEqual(result["rejected_runs"][0]["code"],
                         "TARGET_ACQUISITION_CONTEXT")

        not_fully_current = chain()
        not_fully_current[0]["comparison"] = comparison(
            other_status="UNRESOLVED", joint="UNRESOLVED")
        malformed = event()
        malformed["first_correct"] = {
            key: deepcopy(not_fully_current[0][key])
            for key in ("frame_id", "video_frame_index", "source_frame_seq",
                        "capture_ns", "offset_seconds", "image")}
        result = self.classify(not_fully_current, current_event=malformed)
        self.assertEqual(result["classifications"], [])
        self.assertEqual(result["rejected_runs"][0]["code"],
                         "TARGET_ACQUISITION_CONTEXT")

    def test_first_current_boundary_splits_pre_and_post_refusal_contexts(self):
        samples = [
            sample(10), sample(11),
            sample(12, partial_by_slot={0: (5,)}),
            sample(13), sample(14),
            sample(15, partial_by_slot={0: (5,)}),
            sample(16), sample(17),
        ]
        current_event = event()
        current_event["first_correct"] = {
            key: deepcopy(samples[3][key])
            for key in ("frame_id", "video_frame_index", "source_frame_seq",
                        "capture_ns", "offset_seconds", "image")}
        result = self.classify(samples, current_event=current_event)
        self.assertEqual(result["errors"], [])
        self.assertEqual([record["video_frame_indices"]
                          for record in result["classifications"]], [[15]])
        self.assertEqual(result["classifications"][0]["full_context_indices"],
                         [13, 14, 15, 16, 17])
        self.assertEqual([(item["first"]["video_frame_index"],
                           item["last"]["video_frame_index"], item["code"])
                          for item in result["rejected_runs"]],
                         [(12, 12, "TARGET_ACQUISITION_CONTEXT")])

        no_establishment = event()
        result = self.classify(samples, current_event=no_establishment)
        self.assertEqual(result["classifications"], [])
        self.assertEqual(result["rejected_runs"][0]["code"],
                         "TARGET_ACQUISITION_CONTEXT")

        unbound_establishment = event()
        unbound_establishment["first_correct"] = {
            key: deepcopy(samples[0][key])
            for key in ("frame_id", "video_frame_index", "source_frame_seq",
                        "capture_ns", "offset_seconds", "image")}
        unbound_establishment["first_correct"]["frame_id"] = "not-a-raw-sample"
        result = self.classify(samples, current_event=unbound_establishment)
        self.assertEqual(result["classifications"], [])
        self.assertEqual(result["rejected_runs"][0]["code"],
                         "TARGET_ACQUISITION_CONTEXT")

    def test_context_and_duplicate_evidence_are_bound_exactly(self):
        for changes in [
                {"capture_id": "short"},
                {"selection_manifest_sha256": "short"},
                {"verified_maximum_source_interval_ns": 1_000_000_001},
                {"reader_method_version": 6},
                {"reader_sha256": "d" * 64},
        ]:
            with self.subTest(changes=changes):
                result = self.classify(chain(), current_context=context(**changes))
                self.assertEqual(result["classifications"], [])
                self.assertEqual(len(result["errors"]), 1)

        samples = chain()
        conflict = deepcopy(samples[2])
        conflict["comparison"]["checks"]["main_bars"]["status"] = "DIFFERENCE"
        result = self.classify([*samples, conflict])
        self.assertEqual(result["classifications"], [])
        self.assertIn("conflicting secondary evidence", result["errors"][0])

    def test_unclosed_and_non_candidate_states_are_not_inferred(self):
        unclosed = chain()[2:]
        result = self.classify(unclosed)
        self.assertEqual(result["classifications"], [])
        self.assertEqual(result["rejected_runs"][0]["code"], "UNCLOSED_RUN")

        all_readable = [sample(index) for index in range(10, 16)]
        result = self.classify(all_readable)
        self.assertEqual(result, {"classifications": [], "rejected_runs": [],
                                  "errors": []})


if __name__ == "__main__":
    unittest.main()
