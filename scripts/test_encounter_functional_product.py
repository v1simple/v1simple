#!/usr/bin/env python3
"""Functional deadline evidence, distinct from the old optical coherence rule."""
import copy
import unittest
from unittest.mock import patch

from test_encounter_product import (MS, ANCHOR, make_event, set_observation,
                                    observation_at, qualification, refresh_coverage, TEST_CLASSIFIER,
                                    TEST_CLASSIFIER_SPEC_SHA256)
from encounter_product import judge_visible_event_presentation as judge, load_policy


class FunctionalProductTests(unittest.TestCase):
    def qualified(self, event, point, semantics):
        policy = load_policy()
        policy["qualified_temporal_classifier_ids"] = [TEST_CLASSIFIER]
        policy["qualified_temporal_classifiers"] = {TEST_CLASSIFIER: {
            "classifier_spec_sha256": TEST_CLASSIFIER_SPEC_SHA256,
            "raw_affected_fields": ["main_arrows"],
            "deadline_observation_semantics": semantics,
        }}
        claim = qualification(event, [point["video_frame_index"]])
        with patch("encounter_product.load_policy", return_value=policy):
            return judge([event], temporal_classifications=[claim])

    def test_progress_before_deadline_is_retained_without_a_false_functional_failure(self):
        event = make_event(first_current_ns=90 * MS)
        set_observation(event, 65 * MS, "DEFINITE_OTHER", fields=("main_bars",))
        set_observation(event, 70 * MS, "UNRESOLVED", fields=("primary_frequency",))
        frozen = copy.deepcopy(event)
        result = judge([event])
        self.assertEqual(result["result"], "PASS")
        evaluated = result["events"][0]
        self.assertEqual(evaluated["acquisition_observations"]["raw_status_counts"]["DEFINITE_OTHER"], 1)
        self.assertEqual(evaluated["acquisition_observations"]["first_current_correct"]["capture_ns"], ANCHOR + 90 * MS)
        self.assertEqual(evaluated["verification_end_ns"], ANCHOR + 292 * MS)
        self.assertEqual(evaluated["raw_observations"], event["observations"])
        self.assertEqual(event, frozen)

    def test_early_correct_image_cannot_hide_late_regression_or_shorten_verification(self):
        for offset in (100, 150, 285, 295):
            with self.subTest(offset=offset):
                event = make_event(first_current_ns=20 * MS)
                set_observation(event, offset * MS, "DEFINITE_OTHER", fields=("secondary",))
                result = judge([event])
                self.assertEqual(result["result"], "FAIL")
                self.assertEqual(result["events"][0]["first_decisive_marker"]["capture_ns"], ANCHOR + offset * MS)

    def test_late_target_and_unresolved_deadline_have_distinct_results(self):
        late = make_event(first_current_ns=105 * MS)
        self.assertEqual(judge([late])["result"], "FAIL")
        unknown = copy.deepcopy(late)
        set_observation(unknown, 100 * MS, "UNRESOLVED", fields=("secondary",))
        self.assertEqual(judge([unknown])["result"], "INCONCLUSIVE")

    def test_unresolved_deadline_does_not_move_the_verification_window(self):
        for wrong_offset, expected in ((150, "FAIL"), (300, "INCONCLUSIVE")):
            with self.subTest(wrong_offset=wrong_offset):
                event = make_event()
                set_observation(event, 100 * MS, "UNRESOLVED", fields=("secondary",))
                set_observation(event, wrong_offset * MS, "DEFINITE_OTHER", fields=("secondary",))
                result = judge([event])
                evaluated = result["events"][0]
                # The sole deadline marker anchors the claim even when unreadable.
                # With no in-window failure, its uncertainty cannot be rescued by
                # a later correct frame or worsened by a post-closing discrepancy.
                self.assertEqual(result["result"], expected)
                self.assertEqual(evaluated["verification_end_ns"], ANCHOR + 292 * MS)
                if wrong_offset == 300:
                    self.assertEqual(evaluated["closing_current_correct"]["capture_ns"], ANCHOR + 295 * MS)
                else:
                    self.assertEqual(evaluated["first_decisive_marker"]["capture_ns"], ANCHOR + 150 * MS)

    def test_pre_deadline_phase_does_not_satisfy_verification_phase_coverage(self):
        event = make_event(first_current_ns=20 * MS, phases=("phase-a", "phase-b"))
        set_observation(event, 90 * MS, "CURRENT", phase="phase-b")
        evaluated = judge([event])["events"][0]
        self.assertEqual(evaluated["result"], "INCONCLUSIVE")
        self.assertIn("LEGAL_BLINK_PHASE_NOT_OBSERVED", evaluated["reasons"])
        self.assertEqual(evaluated["observed_joint_state_ids"], ["phase-a"])

    def test_qualified_deadline_phase_does_not_hide_source_or_bracket_gaps(self):
        for missing_offsets in ((90, 95), (175, 180)):
            with self.subTest(missing_offsets=missing_offsets):
                event = make_event()
                event["observations"] = [item for item in event["observations"]
                                         if item["capture_ns"] - ANCHOR
                                         not in [offset * MS for offset in missing_offsets]]
                refresh_coverage(event)
                point = set_observation(event, 100 * MS, "UNRESOLVED", fields=("main_arrows",))
                evaluated = self.qualified(event, point, "LEGAL_PRESENTATION_TRANSITION")["events"][0]
                self.assertEqual(evaluated["result"], "INCONCLUSIVE")
                self.assertIn("SOURCE_MARKER_GAP", evaluated["reasons"])
                if missing_offsets == (90, 95):
                    self.assertFalse(evaluated["response_acquisition"][
                        "deadline_capture_marker_bracket"]["within_limit"])

    def test_closing_guard_requires_correct_content_within_unchanged_time_bound(self):
        for closing_offset, status, expected in ((300, "CURRENT", "PASS"),
                                                 (303, "CURRENT", "INCONCLUSIVE"),
                                                 (300, "DEFINITE_OTHER", "FAIL")):
            with self.subTest(closing_offset=closing_offset, status=status):
                event = make_event()
                point = set_observation(event, 295 * MS, "UNRESOLVED", fields=("main_arrows",))
                closing = set_observation(event, 300 * MS, status, fields=("secondary",)
                                          if status == "DEFINITE_OTHER" else ())
                closing["capture_ns"] = ANCHOR + closing_offset * MS
                refresh_coverage(event)
                evaluated = self.qualified(event, point, "LEGAL_PRESENTATION_TRANSITION")["events"][0]
                self.assertEqual(evaluated["result"], expected)
                if closing_offset == 303:
                    self.assertIn("VERIFICATION_DURATION_NOT_OBSERVED", evaluated["reasons"])

    def test_unqualified_closing_unknown_cannot_be_replaced_by_later_correct_frame(self):
        event = make_event()
        set_observation(event, 295 * MS, "UNRESOLVED", fields=("main_arrows",))
        evaluated = judge([event])["events"][0]
        self.assertEqual(evaluated["result"], "INCONCLUSIVE")
        self.assertIn("UNCLASSIFIED_VISIBLE_INTERVAL", evaluated["reasons"])

    def test_correct_marker_exactly_at_verification_end_closes_the_claim(self):
        for closing_status, expected in (("CURRENT", "PASS"),
                                         ("DEFINITE_OTHER", "FAIL"),
                                         ("UNRESOLVED", "INCONCLUSIVE")):
            with self.subTest(closing_status=closing_status):
                event = make_event()
                point = set_observation(event, 295 * MS, closing_status,
                                        fields=() if closing_status == "CURRENT" else ("secondary",))
                point["capture_ns"] = ANCHOR + 292 * MS
                refresh_coverage(event)
                # A completed guard cannot be invalidated by a later picture.
                # A wrong or unknown boundary picture remains decisive itself.
                if closing_status == "CURRENT":
                    set_observation(event, 300 * MS, "DEFINITE_OTHER", fields=("secondary",))
                evaluated = judge([event])["events"][0]
                self.assertEqual(evaluated["result"], expected)
                if closing_status == "CURRENT":
                    self.assertEqual(evaluated["closing_current_correct"]["capture_ns"], ANCHOR + 292 * MS)

    def test_mixed_results_preserve_each_event_and_known_failure_wins(self):
        passed = make_event(event_id="pass")
        failed = make_event(event_id="fail")
        unknown = make_event(event_id="unknown")
        set_observation(failed, 125 * MS, "UNRESOLVED", fields=("main_arrows",))
        set_observation(failed, 150 * MS, "DEFINITE_OTHER", fields=("secondary",))
        set_observation(unknown, 100 * MS, "UNRESOLVED", fields=("secondary",))
        result = judge([passed, failed, unknown])
        self.assertEqual(result["result"], "FAIL")
        self.assertEqual([e["result"] for e in result["events"]], ["PASS", "FAIL", "INCONCLUSIVE"])
        self.assertEqual(result["counts"], {"required_events": 3, "passed": 1, "failed": 1, "inconclusive": 1})

    def test_qualified_legal_blink_at_deadline_does_not_invent_a_readable_frame(self):
        event = make_event(phases=("phase-a", "phase-b"), alternate_phases=True)
        point = set_observation(event, 100 * MS, "UNRESOLVED", fields=("main_arrows",))
        result = self.qualified(event, point, "LEGAL_PRESENTATION_TRANSITION")
        self.assertEqual(result["result"], "PASS")
        evaluated = result["events"][0]
        self.assertEqual(evaluated["reason_code"], "LEGAL_PHASE_AT_DEADLINE_AND_VERIFIED")
        self.assertEqual(evaluated["first_current_correct"]["capture_ns"], ANCHOR + 105 * MS)
        self.assertEqual(evaluated["verification_end_ns"], ANCHOR + 292 * MS)
        self.assertEqual(observation_at(event, 100 * MS)["raw_status"], "UNRESOLVED")

    def test_target_still_being_acquired_at_deadline_is_not_a_legal_blink(self):
        event = make_event()
        point = set_observation(event, 100 * MS, "UNRESOLVED", fields=("main_arrows",))
        result = self.qualified(event, point, "TARGET_ACQUISITION_TRANSITION")
        self.assertEqual(result["result"], "FAIL")
        self.assertEqual(result["events"][0]["reason_code"], "TARGET_IN_TRANSITION_AT_DEADLINE_CAPTURE")

    def test_missing_source_evidence_prevents_pass_but_does_not_erase_a_proven_failure(self):
        event = make_event()
        event["coverage"]["complete_recorded_frame_coverage"] = False
        self.assertEqual(judge([event])["result"], "INCONCLUSIVE")
        set_observation(event, 150 * MS, "DEFINITE_OTHER", fields=("main_bars",))
        self.assertEqual(judge([event])["result"], "FAIL")

    def test_preexisting_target_can_prove_content_without_claiming_a_causal_response(self):
        event = make_event(first_current_ns=0)
        set_observation(event, -5 * MS, "CURRENT")
        result = judge([event])
        self.assertEqual(result["result"], "PASS")
        self.assertEqual(result["contract"]["pre_deadline_observations"], "diagnostic_only")

    def test_fatal_instrument_identity_still_prevents_every_product_claim(self):
        event = make_event()
        set_observation(event, 150 * MS, "DEFINITE_OTHER", fields=("secondary",))
        result = judge([event], fatal_integrity_errors=["source image hash mismatch"])
        self.assertEqual(result["result"], "INCONCLUSIVE")
        self.assertEqual(result["events"], [])


if __name__ == "__main__":
    unittest.main()
