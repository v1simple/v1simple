#!/usr/bin/env python3
"""Frozen blink differs from a legal still image, without an acquisition deadline."""
from copy import deepcopy
from pathlib import Path
import sys
import unittest

sys.path[:0] = [str(Path(__file__).resolve().parent), str(Path(__file__).resolve().parent / "bench")]
from encounter_behavior_contract import behavior_contract
from encounter_phase_observation import measure_event_phases
from test_encounter_expectation import alert, literals, recording
from test_encounter_sequence import sequence


class PhaseObservationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.contract = behavior_contract(Path(__file__).resolve().parents[1], "ee6b401")
        cls.on = literals()
        cls.off = literals(active_bands=[], main_arrows=[])
        cls.off["counter_glyph"] = {"state": "absent"}

    def event(self, values, times=None, **kwargs):
        times = times or [1.02 + index * .005 for index in range(len(values))]
        inputs = recording([([alert()], [6, 0, 1, 0x24, 0, 12, 12, 0x40])])
        result, *_ = sequence(times, values, inputs=inputs, **kwargs)
        self.assertFalse(result["errors"])
        return result["events"][0]

    def test_wholly_frozen_legal_phase_has_exact_witnesses_and_source_cadence(self):
        event = self.event([deepcopy(self.on) for _ in range(121)])
        result = measure_event_phases(event, self.contract)
        self.assertEqual(result["status"], "DIFFERENCES_FOUND")
        self.assertEqual(result["required_phase_ids"], ["phase-1", "phase-2"])
        self.assertEqual(result["observed_phase_ids"], ["phase-1"])
        self.assertEqual(result["source_toggle_ms"], 96)
        finding = result["findings"][0]
        self.assertEqual(finding["kind"], "blink_phase_held")
        self.assertEqual(finding["first"]["video_frame_index"], 0)
        self.assertEqual(finding["last"]["video_frame_index"], 120)
        self.assertEqual(finding["observed"]["readable_span_ms"], 600)

    def test_valid_alternation_is_measured_without_finding(self):
        values = [deepcopy(self.on if (index * 5 // 96) % 2 == 0 else self.off) for index in range(121)]
        result = measure_event_phases(self.event(values), self.contract)
        self.assertEqual(result["status"], "PHASES_OBSERVED")
        self.assertGreaterEqual(result["alternation_count"], 5)
        self.assertEqual(result["findings"], [])

    def test_blink_freezing_after_valid_alternations_is_not_hidden(self):
        values = [deepcopy(self.on if (index * 5 // 96) % 2 == 0 else self.off) for index in range(80)]
        values += [deepcopy(self.on) for _ in range(80)]
        result = measure_event_phases(self.event(values), self.contract)
        self.assertEqual(result["observed_phase_ids"], ["phase-1", "phase-2"])
        self.assertEqual(result["status"], "DIFFERENCES_FOUND")
        self.assertEqual(len(result["findings"]), 1)
        self.assertGreater(result["findings"][0]["first"]["video_frame_index"], 50)

    def test_unknown_joint_fields_break_the_proof_but_other_fields_do_not(self):
        unknown = deepcopy(self.on)
        unknown["main_arrows"] = {"state": "unreadable", "reason": "partial color"}
        values = [deepcopy(self.on)] * 21 + [unknown] * 21 + [deepcopy(self.on)] * 21
        result = measure_event_phases(self.event(values), self.contract)
        self.assertEqual(result["findings"], [])
        self.assertEqual(result["unresolved_frames"], 21)
        unrelated = deepcopy(self.on)
        unrelated["primary_frequency"] = {"state": "unreadable", "reason": "partial digit"}
        result = measure_event_phases(self.event([unrelated] * 121), self.contract)
        self.assertEqual(result["status"], "DIFFERENCES_FOUND")
        self.assertEqual(result["unresolved_frames"], 0)

    def test_short_sparse_and_gapped_evidence_does_not_declare_a_freeze(self):
        for event in (self.event([self.on] * 20),
                      self.event([self.on] * 3, times=[1.02, 1.5, 2.0]),
                      self.event([self.on] * 121, selected=set(range(21)) | set(range(100, 121)))):
            self.assertEqual(measure_event_phases(event, self.contract)["findings"], [])

    def test_identical_semantic_phases_are_steady_and_unreviewed_source_cannot_assert_freeze(self):
        event = self.event([self.on] * 121)
        event["target"]["joint_states"] = [event["target"]["joint_states"][0]] * 2
        result = measure_event_phases(event, self.contract)
        self.assertEqual(result["required_phase_ids"], ["phase-1"])
        self.assertEqual(result["status"], "STEADY_INPUT")
        self.assertEqual(result["findings"], [])
        unreviewed = deepcopy(self.contract)
        unreviewed["rules"]["shared_blink"]["status"] = "UNREVIEWED"
        result = measure_event_phases(self.event([self.on] * 121), unreviewed)
        self.assertEqual(result["status"], "SOURCE_UNVERIFIED")
        self.assertEqual(result["findings"], [])


if __name__ == "__main__":
    unittest.main()
