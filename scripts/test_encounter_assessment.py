#!/usr/bin/env python3
"""Named encounter answers preserve failures, uncertainty and every denominator."""
import copy
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from bench.encounter_assessment import assess
from bench.encounter_expectation import FIELDS


def sample(number, role="held", changes=None, joint="MATCH"):
    result = {"frame_id": str(number), "video_frame_index": number, "source_frame_seq": number + 1,
              "capture_ns": number * 10, "offset_seconds": number / 100, "image": f"frames/{number}.png",
              "role": role, "observed": {"fields": {name: {"state": "readable", "value": 1} for name in FIELDS}},
              "comparison": {"checks": {name: {"status": "MATCH"} for name in FIELDS},
                             "joint_state": {"status": joint}}}
    result["comparison"]["checks"].update(changes or {})
    return result


def point(sample):
    return {name: sample[name] for name in ("frame_id", "video_frame_index", "source_frame_seq", "capture_ns", "image")}


def event(first, spans=None, end=100):
    return {"event_id": "event-1", "start_ns": 0, "end_ns": end, "first_correct": point(first) if first else None,
            "declared_range_coverage": [{"start_ns": 0, "end_ns": end}],
            "coverage": {"selected_recorded_frames": 1}, "observation_spans": spans or []}


def span(first, status="CORRECT", count=1, wrong=None, unknown=None):
    return {"first": point(first), "last": point(first), "frame_count": count,
            "observed": {"main_arrows": {"state": "ambiguous", "reason": "faint"}},
            "judgment": {"status": status, "joint_state": "MATCH",
                         "not_correct_fields": wrong or [], "unresolved_fields": unknown or []}}


class EncounterAssessmentTests(unittest.TestCase):
    def test_held_failure_survives_unknown_and_successful_event_response(self):
        first = sample(1)
        wrong = sample(2, changes={"main_bars": {"status": "DIFFERENCE", "reason": "wrong strength"},
                                   "main_arrows": {"status": "UNRESOLVED", "reason": "faint"}})
        result = assess([first, wrong], [], {"events": [event(first)]})
        self.assertEqual(result["held"]["status"], "FAIL")
        self.assertEqual(result["event_response"]["status"], "PASS")
        self.assertEqual(result["held"]["required"], 14)
        self.assertEqual(len(result["held"]["issues"]), 2)
        self.assertNotIn("status", result)
        self.assertNotIn("result", result)

    def test_transition_differences_are_retained_without_becoming_held_requirements(self):
        first = sample(1)
        transitional = sample(2, "transition", {"main_bars": {"status": "TRANSITION_DIFFERENCE"},
                                                "main_arrows": {"status": "PREVIOUS_INPUT_STATE"},
                                                "secondary": {"status": "UNRESOLVED", "reason": "partial"}})
        result = assess([first, transitional], [], {"events": [event(first)]})
        self.assertEqual(result["held"]["status"], "PASS")
        self.assertEqual(result["held"]["required"], 7)
        self.assertEqual(result["transitions"]["required"], 7)
        self.assertEqual(result["transitions"]["fields"], {"MATCH": 4, "TRANSITION_DIFFERENCE": 1,
                                                           "PREVIOUS_INPUT_STATE": 1, "UNRESOLVED": 1})
        self.assertEqual(result["transitions"]["joint_states"], {"MATCH": 1})

    def test_grouped_held_blocker_preserves_distinct_reasons_and_first_image(self):
        samples = [sample(1, changes={"main_arrows": {"status": "UNRESOLVED", "reason": "faint"}}),
                   sample(2, changes={"main_arrows": {"status": "UNRESOLVED", "reason": "faint"}}),
                   sample(3, changes={"main_arrows": {"status": "UNRESOLVED", "reason": "partial"}})]
        result = assess(samples, [], {"events": [event(None)]})
        self.assertEqual(result["held"]["status"], "INCONCLUSIVE")
        issues = result["held"]["issues"]
        self.assertEqual([i["reason"] for i in issues], ["faint", "partial"])
        self.assertEqual(issues[0]["frame_ids"], ["1", "2"])
        self.assertEqual(issues[0]["first"]["image"], "frames/1.png")

    def test_missing_event_target_is_not_a_pass_and_is_never_dropped(self):
        first = sample(1)
        missing = event(None)
        missing["event_id"] = "event-2"
        missing["coverage"]["selected_recorded_frames"] = 0
        result = assess([first], [], {"events": [event(first), missing]})
        self.assertEqual(result["event_response"]["status"], "INCONCLUSIVE")
        self.assertEqual(result["event_response"]["required"], 2)
        self.assertEqual(result["event_response"]["observed"], 1)
        self.assertEqual(result["event_response"]["missing_event_ids"], ["event-2"])

    def test_events_outside_declared_observation_scope_are_not_false_failures(self):
        first = sample(1)
        outside = event(None)
        outside["event_id"] = "outside"
        outside["declared_range_coverage"] = []
        outside["coverage"]["selected_recorded_frames"] = 0
        result = assess([first], [], {"events": [event(first), outside]})
        self.assertEqual(result["event_response"]["required"], 1)
        self.assertEqual(result["event_response"]["status"], "PASS")

    def test_no_events_and_no_held_samples_never_create_vacuous_pass(self):
        result = assess([sample(1, "transition")], [], {"events": []})
        self.assertEqual(result["held"]["status"], "NOT_EVALUATED")
        self.assertEqual(result["event_response"]["status"], "INCONCLUSIVE")
        empty = assess([], [], {})
        self.assertEqual(empty["execution"]["status"], "INCOMPLETE")
        self.assertEqual(empty["event_response"]["status"], "INCONCLUSIVE")

    def test_missing_transition_observation_or_selection_error_prevents_ready_answers(self):
        first = sample(1)
        for issue in ("unread", "selection", "comparison"):
            missing = sample(2, "transition")
            if issue == "unread":
                del missing["observed"]
            elif issue == "selection":
                missing["selection_error"] = "no nearby source image"
            else:
                del missing["comparison"]
            result = assess([first, missing], [], {"events": [event(first)]})
            self.assertEqual(result["held"]["status"], "INCONCLUSIVE")
            self.assertEqual(result["event_response"]["status"], "INCONCLUSIVE")
            self.assertEqual(result["execution"]["status"], "INCOMPLETE")
            self.assertEqual(result["execution"]["missing_observation_frames"][0]["frame_id"], "2")
            self.assertEqual(result["transitions"]["required"], 7)

    def test_partial_or_malformed_reader_and_comparison_records_are_incomplete(self):
        first = sample(1)
        mutations = [
            lambda s: s["observed"].update(fields={}),
            lambda s: s["observed"]["fields"].pop("main_arrows"),
            lambda s: s["observed"]["fields"].update(main_arrows={}),
            lambda s: s["comparison"].update(checks={}),
            lambda s: s["comparison"]["checks"].update(main_arrows=None),
            lambda s: s["comparison"]["checks"]["main_arrows"].update(status="unexpected"),
            lambda s: s["comparison"].update(joint_state={}),
            lambda s: s["comparison"].update(joint_state={"status": "unexpected"}),
            lambda s: s.update(comparison=None),
        ]
        for mutation in mutations:
            partial = sample(2, "transition")
            mutation(partial)
            result = assess([first, partial], [], {"events": [event(first)]})
            self.assertEqual(result["execution"]["status"], "INCOMPLETE")
            self.assertEqual(result["event_response"]["status"], "INCONCLUSIVE")
            self.assertEqual(result["transitions"]["required"], 7)
        unreadable = sample(2, "transition", changes={"main_arrows": {"status": "UNRESOLVED"}})
        unreadable["observed"]["fields"]["main_arrows"] = {"state": "unreadable", "reason": "glare"}
        result = assess([first, unreadable], [], {"events": [event(first)]})
        self.assertEqual(result["execution"]["status"], "COMPLETE")
        self.assertEqual(result["transitions"]["fields"]["UNRESOLVED"], 1)

    def test_run_or_sequence_errors_are_not_overridden_by_correct_frames(self):
        first = sample(1)
        for errors, sequence_errors in ((["decoder error"], []), ([], ["bad input order"])):
            result = assess([first], errors, {"events": [event(first)], "errors": sequence_errors})
            self.assertEqual(result["event_response"]["observed"], 1)
            self.assertEqual(result["event_response"]["status"], "INCONCLUSIVE")
            self.assertEqual(result["held"]["status"], "INCONCLUSIVE")
            self.assertTrue(result["errors"])

    def test_first_correct_must_identify_read_original_in_declared_event_range(self):
        first = sample(1)
        for mutate in (lambda e: e.update(end_ns=10),
                       lambda e: e.update(declared_range_coverage=[{"start_ns": 20, "end_ns": 100}]),
                       lambda e: e["first_correct"].update(frame_id="unread"),
                       lambda e: e["first_correct"].update(video_frame_index=20)):
            candidate = event(first)
            mutate(candidate)
            result = assess([first], [], {"events": [candidate]})
            self.assertEqual(result["event_response"]["observed"], 0)
            self.assertEqual(result["event_response"]["status"], "INCONCLUSIVE")

    def test_later_wrong_and_unknown_survive_first_correct_and_count_mixed_span_twice(self):
        first, later, last = sample(1), sample(2, "transition"), sample(3, "transition")
        spans = [span(first), span(later, "NOT_CORRECT", 2, ["primary_frequency"], ["main_arrows"]),
                 span(last, "UNRESOLVED", 1, unknown=["secondary"])]
        result = assess([first, later, last], [], {"events": [event(first, spans)]})
        self.assertEqual(result["event_response"]["status"], "PASS")
        after = result["after_correct"]
        self.assertEqual(after["differing_spans"], 1)
        self.assertEqual(after["unresolved_spans"], 2)
        self.assertEqual(after["differing_frames"], 2)
        self.assertEqual(after["unresolved_frames"], 3)
        self.assertEqual(after["details"][0]["first"]["frame_id"], "2")
        self.assertEqual(after["details"][0]["unresolved_reasons"], {"main_arrows": "faint"})
        self.assertEqual(after["details"][1]["unresolved_fields"], ["secondary"])

    def test_joint_unknown_unsupported_or_failure_is_not_hidden_by_field_matches(self):
        for status in ("UNRESOLVED", "NOT_EVALUATED", "DIFFERENCE"):
            first = sample(1, joint=status)
            result = assess([first], [], {"events": [event(first)]})
            self.assertEqual(result["held"]["status"], "FAIL" if status == "DIFFERENCE" else "INCONCLUSIVE")
            self.assertEqual(result["held"]["issues"][0]["field"], "joint_state")
        later = sample(2, "transition")
        conflicting = span(later)
        conflicting["judgment"]["joint_state"] = "TRANSITION_DIFFERENCE"
        first = sample(1)
        result = assess([first, later], [], {"events": [event(first, [span(first), conflicting])]})
        self.assertEqual(result["after_correct"]["differing_spans"], 1)

    def test_side_effect_free_and_returned_details_do_not_alias_retained_inputs(self):
        first, later = sample(1), sample(2, "transition")
        samples = [first, later]
        sequence = {"events": [event(first, [span(first), span(later, "UNRESOLVED", unknown=["main_arrows"])])]}
        original = copy.deepcopy((samples, sequence))
        result = assess(samples, [], sequence)
        result["after_correct"]["details"][0]["first"]["image"] = "changed"
        self.assertEqual((samples, sequence), original)


if __name__ == "__main__":
    unittest.main()
