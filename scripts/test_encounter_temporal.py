#!/usr/bin/env python3
"""Sequence classifiers preserve raw refusals and reject unclosed guesses."""
from copy import deepcopy
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent / "bench"))
import encounter_temporal as temporal


def card(bars, compatible=None, frequency="24.150", slot=0):
    return {"slot": slot, "band": "K" if slot == 0 else "Ka",
            "frequency": frequency, "direction": "side" if slot == 0 else "rear",
            "bars": bars, "compatible_bars": compatible,
            "bars_state": "readable" if bars is not None else "ambiguous"}


def sample(index, reading, *, frame_seq=None, capture_ns=None):
    return {"frame_id": str(index), "video_frame_index": index,
            "source_frame_seq": index + 1 if frame_seq is None else frame_seq,
            "capture_ns": index * 5_000_000 if capture_ns is None else capture_ns,
            "observed": {"fields": {"secondary": reading}}}


def readable(count=3, frequency="24.150"):
    value = [{"band": "K", "frequency": frequency, "direction": "side", "bars": count}]
    return {"state": "readable", "value": value, "cards": [card(count, [count], frequency)]}


def ambiguous(compatible, frequency="24.150"):
    return {"state": "unreadable", "value": None, "reason": "partial meter",
            "cards": [card(None, compatible, frequency)]}


def two_cards(second_bars, second_compatible=None):
    cards = [card(3, [3]), card(second_bars, second_compatible, "35.500", slot=1)]
    value = [{"band": item["band"], "frequency": item["frequency"],
              "direction": item["direction"], "bars": item["bars"]} for item in cards]
    if second_bars is None:
        return {"state": "unreadable", "value": None, "reason": "partial meter", "cards": cards}
    return {"state": "readable", "value": value, "cards": cards}


def sequence(end_ns=100_000_000):
    return {"events": [{"event_id": "event-0001", "start_ns": 0, "end_ns": end_ns}]}


class EncounterTemporalTests(unittest.TestCase):
    def test_arrow_classifier_records_are_forwarded_without_wrapper_mutation(self):
        samples = [{
            "frame_id": "1", "video_frame_index": 1, "source_frame_seq": 2,
            "capture_ns": 5_000_000,
            "observed": {"fields": {"main_arrows": {"state": "ambiguous"}}},
        }]
        arrow = {
            "classifications": [{"opaque_record": {"value": 1}}],
            "rejected_runs": [{"opaque_rejection": [1, 2]}],
            "errors": [],
        }
        context = {"capture_id": "capture"}
        with patch("encounter_arrow_transition.classify_arrow_runs",
                   return_value=arrow) as classify:
            result = temporal.classify_temporal(
                samples, sequence(), context,
                classifier_ids=[temporal.ARROW_CLASSIFIER_ID])
        classify.assert_called_once_with(samples, sequence()["events"], context)
        self.assertIs(result["classifications"][0], arrow["classifications"][0])
        self.assertIs(result["rejected_runs"][0], arrow["rejected_runs"][0])

    def test_singleton_compatible_count_with_equal_endpoints_is_corroborated(self):
        samples = [sample(1, readable()), sample(2, ambiguous([3])), sample(3, readable())]
        frozen = deepcopy(samples)
        result = temporal.classify_temporal(
            samples, sequence(), classifier_ids=[temporal.SECONDARY_CLASSIFIER_ID])
        self.assertEqual(result["errors"], [])
        self.assertEqual(len(result["classifications"]), 1)
        record = result["classifications"][0]
        self.assertEqual(record["classifier_id"], temporal.SECONDARY_CLASSIFIER_ID)
        self.assertEqual(record["classifier_spec_sha256"],
                         temporal.SECONDARY_CLASSIFIER_SPEC_SHA256)
        self.assertEqual(record["video_frame_indices"], [2])
        self.assertEqual(record["resolved_value"], readable()["value"])
        self.assertEqual(samples, frozen)

    def test_two_frame_intersection_must_uniquely_equal_both_endpoints(self):
        samples = [sample(1, readable()), sample(2, ambiguous([3, 4])),
                   sample(3, ambiguous([2, 3])), sample(4, readable())]
        result = temporal.classify_temporal(
            samples, sequence(), classifier_ids=[temporal.SECONDARY_CLASSIFIER_ID])
        self.assertEqual(len(result["classifications"]), 1)
        self.assertEqual(result["classifications"][0]["video_frame_indices"], [2, 3])
        for candidates in ([3, 4], [2, 3]):
            rejected = temporal.classify_temporal(
                [sample(1, readable()), sample(2, ambiguous(candidates)), sample(3, readable())],
                sequence(), classifier_ids=[temporal.SECONDARY_CLASSIFIER_ID])
            self.assertEqual(rejected["classifications"], [])

    def test_one_ambiguous_meter_can_close_while_other_card_stays_exact(self):
        samples = [sample(1, two_cards(3)), sample(2, two_cards(None, [3, 4])),
                   sample(3, two_cards(None, [2, 3])), sample(4, two_cards(3))]
        result = temporal.classify_temporal(
            samples, sequence(), classifier_ids=[temporal.SECONDARY_CLASSIFIER_ID])
        self.assertEqual(len(result["classifications"]), 1, result)
        self.assertEqual(result["classifications"][0]["resolved_value"], two_cards(3)["value"])

    def test_gap_open_run_changed_identity_and_conflicting_endpoints_reject(self):
        cases = [
            [sample(1, readable()), sample(2, ambiguous([3]), frame_seq=8), sample(3, readable(), frame_seq=9)],
            [sample(1, readable()), sample(2, ambiguous([3], "35.500")), sample(3, readable())],
            [sample(1, readable(3)), sample(2, ambiguous([3])), sample(3, readable(4))],
            [sample(1, ambiguous([3])), sample(2, readable())],
        ]
        for samples in cases:
            with self.subTest(samples=samples):
                result = temporal.classify_temporal(
                    samples, sequence(), classifier_ids=[temporal.SECONDARY_CLASSIFIER_ID])
                self.assertEqual(result["classifications"], [])
                self.assertTrue(result["rejected_runs"])

    def test_definite_wrong_bar_count_is_never_a_temporal_candidate(self):
        samples = [sample(1, readable(3)), sample(2, readable(4)), sample(3, readable(3))]
        result = temporal.classify_temporal(
            samples, sequence(), classifier_ids=[temporal.SECONDARY_CLASSIFIER_ID])
        self.assertEqual(result["classifications"], [])
        self.assertEqual(result["rejected_runs"], [])

    def test_duplicate_source_identity_with_conflicting_pixels_aborts_classification(self):
        samples = [sample(1, readable()), sample(2, ambiguous([3])),
                   {**sample(2, readable(4)), "frame_id": "duplicate"}, sample(3, readable())]
        result = temporal.classify_temporal(
            samples, sequence(), classifier_ids=[temporal.SECONDARY_CLASSIFIER_ID])
        self.assertEqual(result["classifications"], [])
        self.assertTrue(result["errors"])

    def test_capture_gap_endpoint_span_and_malformed_card_shape_reject(self):
        gap = [sample(1, readable(), capture_ns=0),
               sample(2, ambiguous([3]), capture_ns=10_000_001),
               sample(3, readable(), capture_ns=15_000_000)]
        long_span = [sample(1, readable(), capture_ns=0)]
        long_span += [sample(index, ambiguous([3]), capture_ns=(index - 1) * 8_000_000)
                      for index in range(2, 8)]
        long_span.append(sample(8, readable(), capture_ns=56_000_000))
        malformed = [sample(1, readable()), sample(2, ambiguous([3])), sample(3, readable())]
        malformed[1]["observed"]["fields"]["secondary"]["cards"][0]["slot"] = 1
        for samples in (gap, long_span, malformed):
            with self.subTest(samples=samples):
                result = temporal.classify_temporal(
                    samples, sequence(end_ns=100_000_000),
                    classifier_ids=[temporal.SECONDARY_CLASSIFIER_ID])
                self.assertEqual(result["classifications"], [])
                self.assertTrue(result["rejected_runs"])

    def test_unrequested_candidate_failure_is_isolated_but_requested_failure_is_retained(self):
        samples = [{
            "frame_id": "1", "video_frame_index": 1, "source_frame_seq": 2,
            "capture_ns": 5_000_000,
            "observed": {"fields": {
                "main_arrows": {"state": "ambiguous"},
                "main_bars": {"state": "ambiguous"},
            }},
        }]
        arrow = {"classifications": [], "rejected_runs": [], "errors": []}
        broken_bar = {"classifications": [], "rejected_runs": [],
                      "errors": ["candidate bar classifier failed"]}
        with (patch("encounter_arrow_transition.classify_arrow_runs", return_value=arrow),
              patch("encounter_bar_transition.classify_main_bar_runs",
                    return_value=broken_bar) as classify_bar):
            ordinary = temporal.classify_temporal(
                samples, sequence(), {}, classifier_ids=[temporal.ARROW_CLASSIFIER_ID])
        classify_bar.assert_not_called()
        self.assertEqual(ordinary["errors"], [])

        with patch("encounter_bar_transition.classify_main_bar_runs",
                   return_value=broken_bar) as classify_bar:
            requested = temporal.classify_temporal(
                samples, sequence(), {}, classifier_ids=[temporal.BAR_CLASSIFIER_ID])
        classify_bar.assert_called_once()
        self.assertEqual(requested["errors"], ["candidate bar classifier failed"])

    def test_shared_mute_module_cannot_emit_an_unrequested_frequency_candidate(self):
        samples = [{
            "frame_id": "1", "video_frame_index": 1, "source_frame_seq": 2,
            "capture_ns": 5_000_000,
            "observed": {"fields": {
                "muted_badge": {"state": "ambiguous"},
                "primary_frequency": {"state": "ambiguous"},
            }},
        }]
        returned = {
            "classifications": [
                {"classifier_id": temporal.BADGE_CLASSIFIER_ID},
                {"classifier_id": temporal.FREQUENCY_CLASSIFIER_ID},
            ],
            "rejected_runs": [
                {"field": "muted_badge"}, {"field": "primary_frequency"},
            ],
            "errors": [],
        }

        def classify(scoped, _events, _context, *, classifier_ids):
            self.assertEqual(classifier_ids, [temporal.BADGE_CLASSIFIER_ID])
            self.assertEqual(scoped, samples)
            return returned

        with patch("encounter_mute_redraw_transition.classify_mute_redraw_runs",
                   side_effect=classify):
            result = temporal.classify_temporal(
                samples, sequence(), {}, classifier_ids=[temporal.BADGE_CLASSIFIER_ID])
        self.assertEqual(result["classifications"],
                         [{"classifier_id": temporal.BADGE_CLASSIFIER_ID}])
        self.assertEqual(result["rejected_runs"], [{"field": "muted_badge"}])


if __name__ == "__main__":
    unittest.main()
