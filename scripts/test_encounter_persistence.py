#!/usr/bin/env python3
"""Persistence stages must preserve wrong content, missed stages and unknowns."""
import copy
import unittest

from bench.encounter_persistence import measure_persistence_behavior, persistence_result


K = {"band": "K", "frequency": "24.150", "direction": "front", "bars": 3, "priority": True}
X = {"band": "X", "frequency": "10.525", "direction": "rear", "bars": 2, "priority": False}
KA = {"band": "Ka", "frequency": "34.700", "direction": "side", "bars": 5, "priority": True}
CONFIG = {"alertPersistenceSeconds": 2, "stealthEnabled": False}


def card(row):
    return {key: row[key] for key in ("band", "frequency", "direction", "bars")}


def span(number, frequency="--.---", counter="L", bars=0, cards=None, **extra):
    values = dict(primary_frequency=frequency, counter_glyph=counter, main_bars=bars,
                  secondary=cards or [], active_bands=[], main_arrows=[], muted_badge=False)
    values.update(extra)
    point = {"capture_ns": number * 1_000_000, "video_frame_index": number,
             "image": f"frames/{number}.png"}
    return {"first": point, "last": point, "frame_count": 1,
            "observed": {k: {"state": "readable", "value": v} for k, v in values.items()}}


def event(number, rows, spans, duration=8):
    primary = next((r for r in rows if r["priority"]), None)
    fields = dict(primary_frequency=primary["frequency"] if primary else "--.---",
                  counter_glyph=str(len(rows)) if rows else "L", main_bars=6 if primary else 0,
                  secondary=[card(r) for r in rows if not r["priority"]],
                  active_bands=[primary["band"]] if primary else [],
                  main_arrows=[primary["direction"]] if primary else [], muted_badge=False)
    return {"event_id": f"event-{number}", "wire_rows": rows, "start_ns": 1, "end_ns": 8_000_000_001,
            "input_key": {"authored_offsets_seconds": [0], "next_authored_offset_seconds": duration},
            "observation": {"input_anchor_ns": 1}, "observation_spans": spans,
            "target": {"fields": {k: {"allowed": [v]} for k, v in fields.items()},
                       "joint_states": [{k: fields[k] for k in ("counter_glyph", "active_bands", "main_arrows")}],
                       "secondary_policy": {"required": fields["secondary"], "previously_seen": [card(K), card(X), card(KA)]}},
            "coverage": {"complete_recorded_frame_coverage": True, "unrecorded_source_frames": 0}}


def live(number, row=KA, cards=None):
    return span(number, row["frequency"], "1", 6, cards,
                active_bands=[row["band"]], main_arrows=[row["direction"]])


class PersistenceTests(unittest.TestCase):
    def measure(self, events):
        return measure_persistence_behavior(events, CONFIG)

    def test_primary_retention_then_clear_has_exact_originals_and_observed_timing(self):
        result = self.measure([event(1, [K], []), event(2, [], [span(1, "24.150"), span(2)])])
        self.assertEqual(result["result"], "NO_DIFFERENCES_OBSERVED")
        case = result["cases"][0]
        self.assertEqual(case["observed_stages"], ["retained_primary", "cleared"])
        self.assertEqual(case["stages"]["cleared"]["first"]["image"], "frames/2.png")
        self.assertNotIn("deadline", case)

    def test_never_seen_retention_is_measurement_gap_and_not_a_fabricated_timing_failure(self):
        result = self.measure([event(1, [K], []), event(2, [], [span(1), span(2)])])
        self.assertEqual(result["result"], "MEASUREMENT_INCOMPLETE")
        self.assertEqual(result["cases"][0]["missing_stages"], ["retained_primary"])
        self.assertEqual(result["summary"]["findings"], 0)

    def test_never_clearing_old_primary_is_reported_with_ending_evidence(self):
        result = self.measure([event(1, [K], []), event(2, [], [span(1, "24.150"), span(8000, "24.150")])])
        self.assertEqual(result["result"], "DIFFERENCES_FOUND")
        self.assertEqual(result["cases"][0]["findings"][0]["last"]["image"], "frames/8000.png")

    def test_unreadable_suffix_does_not_turn_earlier_retention_into_an_ending_failure(self):
        unknown = span(3)
        unknown["observed"]["primary_frequency"] = {"state": "unreadable", "reason": "faint"}
        result = self.measure([event(1, [K], []), event(2, [], [span(1, "24.150"), unknown])])
        self.assertEqual(result["result"], "MEASUREMENT_INCOMPLETE")
        self.assertEqual(result["summary"]["findings"], 0)

    def test_new_live_primary_preempts_retained_primary_and_wrong_priority_is_caught(self):
        events = [event(1, [K], []), event(2, [], [span(1, "24.150")], duration=1),
                  event(3, [KA], [live(3)])]
        self.assertEqual(self.measure(events)["result"], "NO_DIFFERENCES_OBSERVED")
        events[-1]["observation_spans"] = [live(3, K)]
        self.assertEqual(self.measure(events)["result"], "DIFFERENCES_FOUND")

    def test_retired_card_clears_while_live_primary_remains(self):
        events = [event(1, [K], []), event(2, [KA], [live(1, cards=[card(K)]), live(2)])]
        self.assertEqual(self.measure(events)["result"], "NO_DIFFERENCES_OBSERVED")
        events[-1]["observation_spans"].append(live(3, cards=[card(K)]))
        self.assertEqual(self.measure(events)["result"], "DIFFERENCES_FOUND")

    def test_short_authored_card_hold_does_not_demand_premature_expiry(self):
        events = [event(1, [K], []), event(2, [KA], [live(1, cards=[card(K)])], duration=.5)]
        self.assertEqual(self.measure(events)["result"], "NO_DIFFERENCES_OBSERVED")
        self.assertEqual(self.measure(events)["cases"][0]["required_stages"], ["retained_card_with_live"])

    def test_nonradar_and_stealth_presentation_are_explicitly_outside_scope(self):
        laser = {**K, "band": "Laser"}
        result = self.measure([event(1, [laser], [])])
        self.assertEqual(result["result"], "MEASUREMENT_INCOMPLETE")
        self.assertIn("X/K/Ka", result["reason"])
        self.assertEqual(measure_persistence_behavior([], {**CONFIG, "stealthEnabled": True})["result"], "MEASUREMENT_INCOMPLETE")

    def test_initial_clear_before_retirement_is_not_mistaken_for_a_completed_clear(self):
        events = [event(1, [K], []), event(2, [KA], [live(1), live(2, cards=[card(K)]), live(3)])]
        self.assertEqual(self.measure(events)["result"], "NO_DIFFERENCES_OBSERVED")

    def test_partial_card_cannot_prove_retention_or_clear(self):
        partial = card(K)
        partial["frequency"] = None
        result = self.measure([event(1, [K], []), event(2, [KA], [live(1, cards=[partial])])])
        self.assertEqual(result["result"], "MEASUREMENT_INCOMPLETE")
        self.assertEqual(result["summary"]["unresolved_frames"], 1)

    def test_missing_live_card_and_wrong_association_are_not_excused_as_retirement(self):
        event_value = event(2, [KA, X], [live(1)])
        event_value["observation_spans"][0]["observed"]["counter_glyph"]["value"] = "2"
        result = self.measure([event(1, [KA], []), event_value])
        self.assertEqual(result["result"], "DIFFERENCES_FOUND")

    def test_unrelated_historical_card_does_not_prove_the_requested_retirement(self):
        result = self.measure([event(1, [K], []),
                               event(2, [KA], [live(1, cards=[card(X)]), live(2)])])
        self.assertEqual(result["result"], "MEASUREMENT_INCOMPLETE")
        self.assertIn("retained_card_with_live", result["cases"][0]["missing_stages"])

    def test_missing_recorded_frame_coverage_prevents_completion(self):
        events = [event(1, [K], []), event(2, [], [span(1, "24.150"), span(2)])]
        events[-1]["coverage"]["unrecorded_source_frames"] = 1
        self.assertEqual(self.measure(events)["result"], "MEASUREMENT_INCOMPLETE")

    def test_configuration_and_input_are_never_mutated(self):
        events = [event(1, [K], []), event(2, [], [span(1, "24.150"), span(2)])]
        before = copy.deepcopy(events)
        self.measure(events)
        self.assertEqual(events, before)
        self.assertEqual(measure_persistence_behavior(events, {"alertPersistenceSeconds": 0})["result"],
                         "MEASUREMENT_INCOMPLETE")

    def test_initial_idle_requires_a_readable_cleared_display(self):
        good = event(1, [], [span(1)])
        self.assertEqual(self.measure([good])["result"], "NO_DIFFERENCES_OBSERVED")
        good["observation_spans"][0]["observed"]["primary_frequency"]["value"] = "24.150"
        self.assertEqual(self.measure([good])["result"], "DIFFERENCES_FOUND")


class PersistenceResultTests(unittest.TestCase):
    def setUp(self):
        self.events = [event(1, [], [span(1)]), event(2, [K], []),
                       event(3, [], [span(1, "24.150"), span(2)])]
        self.events[1]["observation"]["target_observed"] = True
        self.events[1]["phase_observation"] = {"required_phase_ids": ["phase-1"], "observed_phase_ids": ["phase-1"]}
        self.measurement = measure_persistence_behavior(self.events, CONFIG)

    def result(self, errors=(), qualification=None):
        return persistence_result(self.events, errors, qualification or {"status": "QUALIFIED"}, self.measurement)

    def test_dynamic_idle_does_not_require_one_fixed_seven_field_target(self):
        self.assertEqual(self.result(), "NO_DIFFERENCES_OBSERVED")

    def test_unobserved_live_seed_is_not_hidden_by_complete_retirement(self):
        self.events[1]["observation"]["target_observed"] = False
        self.assertEqual(self.result(), "MEASUREMENT_INCOMPLETE")

    def test_fully_measured_live_fields_with_permitted_retired_card_cover_short_hold(self):
        short = event(3, [KA], [live(1, cards=[card(K)])], duration=.5)
        short["observation"]["target_observed"] = False
        short["phase_observation"] = {"required_phase_ids": ["phase-1"], "observed_phase_ids": ["phase-1"]}
        self.events[-1] = short
        self.measurement = measure_persistence_behavior(self.events, CONFIG)
        self.assertEqual(self.result(), "NO_DIFFERENCES_OBSERVED")

    def test_missing_live_blink_phase_is_not_hidden_by_complete_retirement(self):
        self.events[1]["phase_observation"]["required_phase_ids"].append("phase-2")
        self.assertEqual(self.result(), "MEASUREMENT_INCOMPLETE")

    def test_ordinary_finding_still_controls_the_result(self):
        self.events[1]["findings"] = [{"kind": "wrong_frequency"}]
        self.assertEqual(self.result(), "DIFFERENCES_FOUND")

    def test_errors_reader_rejection_and_any_missing_coverage_prevent_completion(self):
        self.assertEqual(self.result(errors=["reader failed"]), "MEASUREMENT_INCOMPLETE")
        self.assertEqual(self.result(qualification={"status": "REJECTED"}), "MEASUREMENT_INCOMPLETE")
        self.events[1]["coverage"]["complete_recorded_frame_coverage"] = False
        self.assertEqual(self.result(), "MEASUREMENT_INCOMPLETE")

    def test_idle_without_a_sequence_measurement_prevents_completion(self):
        self.measurement["cases"] = [case for case in self.measurement["cases"] if case["kind"] != "initial_idle"]
        self.assertEqual(self.result(), "MEASUREMENT_INCOMPLETE")


if __name__ == "__main__":
    unittest.main()
