#!/usr/bin/env python3
"""Literal display sequences retain timing uncertainty and brief wrong content."""
import copy
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from bench.encounter_expectation import build_encounter_timeline
from bench.encounter_sequence import interpret_sequence
from test_encounter_expectation import alert, literals, packet, recording, single


MS = 1_000_000


def camera(times, missing=()):
    return [dict(status="writer_dropped" if index in missing else "written", frame_seq=index + 1,
                 host_capture_ns=round(time * 1e9), duration_ns=5 * MS)
            for index, time in enumerate(times)]


def samples(rows, values, selected=None):
    written = [r for r in rows if r["status"] == "written"]
    return [dict(frame_id=f"{index + 1:04d}", video_frame_index=index,
                 capture_ns=row["host_capture_ns"], source_frame_seq=row["frame_seq"],
                 offset_seconds=(row["host_capture_ns"] - 1_000_000_000) / 1e9,
                 observed={"fields": value}) for index, (row, value) in enumerate(zip(written, values))
            if selected is None or index in selected]


def sequence(times, values, inputs=None, selected=None, config=None, missing=()):
    timeline = build_encounter_timeline(*(inputs or single()))
    rows = camera(times, missing)
    observed = samples(rows, values, selected)
    return interpret_sequence(observed, timeline, rows, config), observed, timeline, rows


class EncounterSequenceTests(unittest.TestCase):
    def test_one_frame_wrong_before_correct_retained_and_host_send_bounds_not_onset(self):
        result, observed, timeline, rows = sequence([1.02, 1.025, 1.03], [
            literals(main_bars=4), literals(), literals()])
        event = result["events"][0]
        self.assertEqual(result["errors"], [])
        self.assertEqual(event["first_correct"]["capture_ns"], 1_025_000_000)
        self.assertEqual(event["last_definite_not_correct_before_first"]["capture_ns"], 1_020_000_000)
        self.assertEqual(event["observation_spans"][0]["frame_count"], 1)
        self.assertEqual(event["timing"]["host_send_to_first_correct_capture_ms"], [10, 10.000001])
        self.assertIsNone(event["timing"]["physical_appearance_interval"])
        self.assertTrue(event["timing"]["complete_recorded_frame_prefix"])
        self.assertEqual(event["first_correct_by_field"]["main_bars"]["capture_ns"], 1_025_000_000)
        self.assertEqual(event["first_correct_by_field"]["primary_frequency"]["capture_ns"], 1_020_000_000)
        frozen = copy.deepcopy((observed, timeline, rows))
        interpret_sequence(observed, timeline, rows)
        self.assertEqual((observed, timeline, rows), frozen)

    def test_one_frame_disappearance_after_correct_is_not_hidden_by_unknown(self):
        wrong = literals()
        wrong["primary_frequency"] = dict(state="absent")
        wrong["main_arrows"] = dict(state="unreadable", reason="glare")
        result, *_ = sequence([1.02, 1.025, 1.03], [literals(), wrong, literals()])
        event = result["events"][0]
        self.assertEqual(event["observation_counts"], {"CORRECT": 2, "NOT_CORRECT": 1})
        self.assertEqual(len(event["not_correct_after_correct"]), 1)
        change = event["not_correct_after_correct"][0]
        self.assertEqual(change["not_correct_fields"], ["primary_frequency"])
        self.assertEqual(change["unresolved_fields"], ["main_arrows"])
        self.assertEqual(event["observation_spans"][change["span_index"]]["frame_count"], 1)

    def test_unknown_frames_do_not_carry_forward_correctness(self):
        unclear = literals()
        unclear["primary_frequency"] = dict(state="ambiguous", reason="partially drawn digit")
        result, *_ = sequence([1.02, 1.025, 1.03], [literals(), unclear, literals()])
        event = result["events"][0]
        self.assertEqual([s["judgment"]["status"] for s in event["observation_spans"]],
                         ["CORRECT", "UNRESOLVED", "CORRECT"])
        self.assertEqual(event["not_correct_after_correct"], [])
        self.assertEqual(len(event["changes_after_correct"]), 2)

    def test_unsampled_and_dropped_frames_break_spans_and_deny_continuity(self):
        result, *_ = sequence([1.02, 1.025, 1.03, 1.035], [literals()] * 3,
                              selected={0, 2}, missing={1})
        event = result["events"][0]
        self.assertEqual(len(event["observation_spans"]), 2)
        self.assertEqual(event["gaps"][0]["unread_recorded_frames"], 1)
        self.assertEqual(event["gaps"][0]["unobserved_source_sequence_positions"], 2)
        self.assertEqual(event["coverage"]["unrecorded_source_frames"], 1)
        self.assertFalse(event["coverage"]["complete_recorded_frame_coverage"])

    def test_pending_input_cannot_establish_first_correct_even_when_pixels_agree(self):
        result, *_ = sequence([1.012, 1.014, 1.02], [literals()] * 3)
        event = result["events"][0]
        self.assertEqual(event["first_correct"]["capture_ns"], 1_020_000_000)
        self.assertEqual(event["observation_counts"], {"INPUT_UNRESOLVED": 2, "CORRECT": 1})
        self.assertIsNone(event["last_definite_not_correct_before_first"])

    def test_no_response_is_never_observed_correct_not_claimed_missing_alert(self):
        result, *_ = sequence([1.02, 1.025, 1.03], [literals(primary_frequency=None)] * 3,
                              selected={0, 2})
        event = result["events"][0]
        self.assertEqual(event["outcome"], "CORRECT_NOT_OBSERVED")
        self.assertEqual(event["timing"]["status"], "unavailable")
        self.assertIn("does not establish a missed alert", event["summary"])
        self.assertFalse(event["coverage"]["complete_recorded_frame_coverage"])

    def test_superseding_input_never_supplies_previous_events_correct_frame(self):
        data = recording([([alert()], [6, 6, 1, 0x24, 0x24, 12, 12, 0x40]),
                          ([alert(frequency=24200)], [6, 6, 1, 0x24, 0x24, 12, 12, 0x40])])
        result, *_ = sequence([1.5, 2.02, 2.025], [literals(primary_frequency=None),
                              literals(primary_frequency="24.200"), literals(primary_frequency="24.200")], inputs=data)
        first, second = result["events"]
        self.assertIsNone(first["first_correct"])
        self.assertEqual(first["end_ns"], 2_000_000_000)
        self.assertEqual(first["end_reason"], "next_changed_input_requested")
        self.assertEqual(second["first_correct"]["capture_ns"], 2_020_000_000)
        self.assertEqual(second["changed_fields"], ["primary_frequency"])

    def test_mute_target_requires_second_accepted_display_not_future_correctness(self):
        data = recording([([alert()], [6, 6, 1, 0x34, 0x34, 13, 12, 0x40])] * 3)
        result, *_ = sequence([1.02, 1.03, 2.02, 2.03, 3.02], [literals(muted_badge=True)] * 5, inputs=data)
        self.assertEqual(result["errors"], [])
        self.assertEqual(len(result["events"]), 1)
        event = result["events"][0]
        self.assertEqual(event["target_basis"]["first_complete_target_stimulus_sequence"], 2)
        self.assertEqual(event["first_correct"]["capture_ns"], 2_020_000_000)
        self.assertEqual(event["observation_counts"], {"INPUT_IN_PROGRESS": 2, "CORRECT": 3})
        self.assertEqual(event["timing"]["host_send_to_first_correct_capture_ms"], [5, 5.000001])
        self.assertEqual(event["timing"]["request_to_first_correct_capture_ms"], 1020)

    def test_permitted_blink_is_a_retained_change_not_disappearance_error(self):
        data = recording([([alert()], [6, 0, 1, 0x24, 0, 12, 12, 0x40])])
        off = literals(active_bands=[], main_arrows=[])
        off["counter_glyph"] = dict(state="absent")
        result, *_ = sequence([1.02, 1.025, 1.03], [literals(), off, literals()], inputs=data)
        event = result["events"][0]
        self.assertEqual(event["observation_counts"], {"CORRECT": 3})
        self.assertEqual(len(event["observation_spans"]), 3)
        self.assertEqual(event["not_correct_after_correct"], [])

    def test_individually_permitted_blink_fields_in_wrong_combination_are_retained(self):
        data = recording([([alert()], [6, 0, 1, 0x24, 0, 12, 12, 0x40])])
        conflict = literals(main_arrows=[])
        result, *_ = sequence([1.02, 1.025, 1.03], [literals(), conflict, literals()], inputs=data)
        event = result["events"][0]
        self.assertEqual(len(event["not_correct_after_correct"]), 1)
        self.assertEqual(event["observation_spans"][1]["judgment"]["joint_state"], "TRANSITION_DIFFERENCE")

    def test_unfinished_image_remains_unread_and_in_denominator(self):
        result, observed, timeline, rows = sequence([1.02, 1.025], [literals()] * 2)
        del observed[1]["observed"]
        event = interpret_sequence(observed, timeline, rows)["events"][0]
        self.assertEqual(event["observation_counts"], {"CORRECT": 1, "UNREAD": 1})
        self.assertEqual(event["coverage"]["selected_recorded_frames"], 2)
        self.assertEqual(event["coverage"]["read_recorded_frames"], 1)
        self.assertFalse(event["coverage"]["complete_recorded_frame_coverage"])

    def test_unsupported_configuration_remains_unresolved(self):
        idle = recording([([], [56, 56, 0, 0, 0, 12, 12, 0x40])])
        result, *_ = sequence([1.02, 1.025], [literals(counter_glyph="L", primary_frequency=None,
            main_bars=0, active_bands=[], main_arrows=[])] * 2, inputs=idle)
        event = result["events"][0]
        self.assertIsNone(event["first_correct"])
        self.assertIn("primary_frequency", event["observation_spans"][0]["judgment"]["unresolved_fields"])

    def test_unscoped_request_ends_event_before_later_visible_content(self):
        _, observed, timeline, rows = sequence([1.02, 1.025, 1.03],
                                              [literals(main_bars=4), literals(), literals()])
        timeline["accepted"].append(dict(global_tx_sequence=3, packet_id=0x43,
            payload_hex=packet(0x43, [0] * 7), stimulus_sequence=None,
            display_requested_ns=1_023_000_000, display_attempted_ns=1_024_000_000,
            display_accepted_ns=1_024_000_001))
        event = interpret_sequence(observed, timeline, rows)["events"][0]
        self.assertEqual(event["end_reason"], "unscoped_input_requested")
        self.assertIsNone(event["first_correct"])
        self.assertEqual(event["observation_counts"], {"NOT_CORRECT": 1})

    def test_state_superseded_before_accepted_has_no_supported_target(self):
        data = recording([([alert()], [6, 6, 1, 0x24, 0x24, 12, 12, 0x40]),
                          ([alert(frequency=24200)], [6, 6, 1, 0x24, 0x24, 12, 12, 0x40])])
        _, observed, timeline, rows = sequence([1.5, 2.02, 2.2], [literals()] * 3, inputs=data)
        for source in (timeline["states"], timeline["stimuli"]):
            source[0]["all_accepted_ns"] = 2_100_000_000
        for p in timeline["accepted"]:
            if p["stimulus_sequence"] == 1:
                p["display_attempted_ns"] += 1_085_000_000
                p["display_accepted_ns"] += 1_085_000_000
        timeline["accepted"].sort(key=lambda p: (p["display_accepted_ns"], p["global_tx_sequence"]))
        result = interpret_sequence(observed, timeline, rows)
        self.assertEqual(result["errors"], [])
        event = result["events"][0]
        self.assertIsNone(event["target"])
        self.assertIsNone(event["first_correct"])
        self.assertEqual(event["observation_counts"], {"INPUT_UNRESOLVED": 1})

    def test_bad_inputs_or_sample_identity_return_no_temporal_claims(self):
        _, observed, timeline, rows = sequence([1.02, 1.025], [literals()] * 2)
        for mutation in (
            lambda t, r, s: t["states"].append(copy.deepcopy(t["states"][0])),
            lambda t, r, s: t["accepted"].pop(),
            lambda t, r, s: t["accepted"].reverse(),
            lambda t, r, s: t["states"][0].pop("all_accepted_ns"),
            lambda t, r, s: r.reverse(),
            lambda t, r, s: s[0].update(capture_ns=3),
            lambda t, r, s: s.append({**s[0], "observed": {"fields": literals(main_bars=4)}}),
        ):
            t, r, s = copy.deepcopy((timeline, rows, observed))
            mutation(t, r, s)
            result = interpret_sequence(s, t, r)
            self.assertEqual(result["events"], [])
            self.assertTrue(result["errors"])

    def test_declared_short_window_does_not_certify_unobserved_event_remainder(self):
        _, observed, timeline, rows = sequence([1.02, 1.025, 1.03, 1.5], [literals()] * 4,
                                              selected={0, 1, 2})
        event = interpret_sequence(observed, timeline, rows, ranges=[(1_020_000_000, 1_040_000_000)])["events"][0]
        self.assertTrue(event["declared_range_coverage"][0]["complete_recorded_frame_coverage"])
        self.assertFalse(event["coverage"]["complete_recorded_frame_coverage"])
        self.assertEqual(event["coverage"]["unread_recorded_frames"], 1)


if __name__ == "__main__":
    unittest.main()
