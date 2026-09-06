#!/usr/bin/env python3
"""The report must preserve visible contradictions and uncertainty through time."""
import copy
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from bench.encounter_expectation import FIELDS
from bench.encounter_observation import summarize_event_observations


def point(number):
    return {"frame_id": str(number), "video_frame_index": number, "source_frame_seq": number + 7,
            "capture_ns": number * 5_000_000, "image": f"frames/{number}.png"}


def span(number, status="MATCH", value=None, last=None, field="main_arrows"):
    last = number if last is None else last
    fields = {name: "MATCH" for name in FIELDS}
    fields[field] = status
    return {"first": point(number), "last": point(last), "frame_count": last - number + 1,
            "observed": {name: {"state": "ambiguous" if fields[name] == "UNRESOLVED" else "readable",
                                "value": value if name == field else 1} for name in FIELDS},
            "judgment": {"status": "CORRECT" if status == "MATCH" else "UNRESOLVED"
                         if status == "UNRESOLVED" else "NOT_CORRECT", "fields": fields}}


def event(spans, first=4):
    return {"event_id": "event-1", "start_ns": 0, "end_ns": 100_000_000,
            "target_basis": {"first_complete_target_input_ns": 5_000_000},
            "first_correct": point(first) if first is not None else None,
            "observation_spans": spans, "gaps": [],
            "coverage": {"complete_recorded_frame_coverage": False}}


class EncounterObservationTests(unittest.TestCase):
    def test_faint_interval_remains_between_different_and_matching_originals(self):
        raw = event([span(1, "PREVIOUS_INPUT_STATE", ["front"]),
                     span(2, "UNRESOLVED", None, last=3), span(4, value=["side"], last=6)])
        untouched = copy.deepcopy(raw)
        result = summarize_event_observations(raw)
        field = result["fields"]["main_arrows"]
        self.assertEqual(raw, untouched)
        self.assertTrue(field["target_observed"])
        bracket = field["first_target_capture_bracket"]
        self.assertEqual(bracket["last_definite_difference"]["video_frame_index"], 1)
        self.assertEqual(bracket["preceding_observation"]["video_frame_index"], 3)
        self.assertEqual(bracket["intervening_unresolved_frames"], 2)
        self.assertEqual(bracket["marker_separation_ms"], 15)
        self.assertEqual(field["counts"], {"matching_frames": 3, "different_frames": 1, "unresolved_frames": 2})
        self.assertNotIn("result", result)
        self.assertNotIn("deadline", result)

    def test_first_match_does_not_hide_later_persistent_wrong_card_or_unknown_suffix(self):
        raw = event([span(1, value=[], field="secondary"),
                     span(2, "DIFFERENCE", ["Ka34.700"], last=4, field="secondary"),
                     span(5, "UNRESOLVED", field="secondary"),
                     span(6, "DIFFERENCE", ["Ka34.700"], field="secondary"),
                     span(7, "UNRESOLVED", last=9, field="secondary")], first=1)
        field = summarize_event_observations(raw)["fields"]["secondary"]
        self.assertTrue(field["target_observed"])
        self.assertEqual(len(field["post_target_departures"]), 2)
        self.assertEqual(field["repeated_contrary_literals_after_target"][0]["frame_count"], 4)
        self.assertEqual(field["latest_contrary_observation"]["video_frame_index"], 6)
        self.assertFalse(field["end_state"]["last_definite_matches_target"])
        self.assertEqual(field["end_state"]["last_observation"]["video_frame_index"], 9)
        self.assertEqual(field["end_state"]["unresolved_suffix"][0]["frame_count"], 3)

    def test_legal_matched_blink_phases_do_not_become_departures(self):
        field = summarize_event_observations(event([
            span(1, value=["front"]), span(2, value=[]), span(3, value=["front"])
        ], first=1))["fields"]["main_arrows"]
        self.assertEqual(field["counts"]["matching_frames"], 3)
        self.assertEqual(field["post_target_departures"], [])
        self.assertTrue(field["end_state"]["last_definite_matches_target"])

    def test_preceding_literal_is_retained_without_inventing_input_caused_change(self):
        raw = event([span(1, value=["front"])], first=1)
        raw["preceding_observation"] = {**point(0), "observed": {"main_arrows": {
            "state": "readable", "value": ["front"]}}}
        raw["target_already_correct_in_preceding_observation"] = True
        result = summarize_event_observations(raw)
        bracket = result["fields"]["main_arrows"]["first_target_capture_bracket"]
        self.assertTrue(result["target_already_correct_in_preceding_observation"])
        self.assertEqual(bracket["preceding_observation"]["observed"]["value"], ["front"])
        self.assertIsNone(bracket["last_definite_difference"])
        self.assertIsNone(bracket["marker_separation_ms"])

    def test_unread_images_and_missing_target_never_become_absence_or_success(self):
        unread = span(1, "UNRESOLVED", last=4)
        unread["judgment"] = {"status": "UNREAD", "fields": {}}
        unread["observed"] = {}
        field = summarize_event_observations(event([unread], first=None))["fields"]["main_arrows"]
        self.assertFalse(field["target_observed"])
        self.assertEqual(field["counts"]["unresolved_frames"], 4)
        self.assertIsNone(field["end_state"]["last_definite_matches_target"])
        self.assertIsNone(field["first_target_capture_bracket"])

    def test_input_in_progress_and_unresolved_input_do_not_count_as_content_contradictions(self):
        pending = span(0, "DIFFERENCE", ["front"])
        pending["judgment"]["status"] = "INPUT_IN_PROGRESS"
        unknown = span(1)
        # A literal can happen to equal the target while its input is unresolved.
        # The parent sequence status must keep that comparison unavailable.
        unknown["judgment"]["status"] = "INPUT_UNRESOLVED"
        field = summarize_event_observations(event([pending, unknown], first=None))["fields"]["main_arrows"]
        self.assertEqual(field["counts"], {"matching_frames": 0, "different_frames": 0, "unresolved_frames": 1})

    def test_unsampled_gap_cannot_become_a_tight_appearance_bracket(self):
        raw = event([span(1, "DIFFERENCE", ["front"]), span(8, value=["side"])], first=8)
        raw["gaps"] = [{"before": point(1), "after": point(8), "unread_recorded_frames": 6}]
        result = summarize_event_observations(raw)
        bracket = result["fields"]["main_arrows"]["first_target_capture_bracket"]
        self.assertEqual(bracket["marker_separation_ms"], 35)
        self.assertEqual(bracket["recorded_frame_gaps"][0]["unread_recorded_frames"], 6)
        self.assertFalse(result["coverage"]["complete_recorded_frame_coverage"])

    def test_arrow_color_evidence_is_literal_and_never_carried_across_a_gap(self):
        samples = []
        for number in (1, 2, 4):
            samples.append({**point(number), "observed": {"fields": {"main_arrows": {
                "state": "ambiguous", "visible_directions": ["side"],
                "direction_states": {"front": {"state": "faint", "rgb_median": [23, 14, 13]},
                                     "side": {"state": "filled"}, "rear": {"state": "unlit"}}}}}})
        unchanged = copy.deepcopy(samples)
        result = summarize_event_observations(event([]), samples)
        intervals = result["arrow_partial_or_faint_intervals"]
        self.assertEqual(samples, unchanged)
        self.assertEqual([item["frame_count"] for item in intervals], [2, 1])
        self.assertEqual(intervals[0]["last"]["visible_directions"], ["side"])
        self.assertEqual(intervals[0]["last"]["rgb_median"], [23, 14, 13])


if __name__ == "__main__":
    unittest.main()
