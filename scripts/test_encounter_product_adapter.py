#!/usr/bin/env python3
"""Adversarial tests for the exact-window product input adapter."""

import copy
from pathlib import Path
import sys
import unittest
from unittest.mock import patch


sys.path.insert(0, str(Path(__file__).resolve().parent / "bench"))
import encounter_product_adapter as adapter
from encounter_expectation import FIELDS
from encounter_product import judge_visible_event_presentation, load_policy


MS = 1_000_000
ANCHOR = 1_000_000_000


def target(*, frequency="24.200", bars=2, arrows=("front",), unresolved=()):
    values = {
        "counter_glyph": "1",
        "primary_frequency": frequency,
        "active_bands": ["K"],
        "main_arrows": list(arrows),
        "main_bars": bars,
        "secondary": [],
        "muted_badge": False,
    }
    fields = {name: {"allowed": [value]} for name, value in values.items()}
    for name in unresolved:
        fields[name] = {"unresolved": "fixture deliberately unresolved"}
    phase = {name: values[name] for name in ("counter_glyph", "active_bands", "main_arrows")}
    return {"fields": fields, "joint_states": [phase],
            "secondary_policy": {"required": [], "previously_seen": [],
                                 "retirement_unknown": False}}


def observed(*, frequency="24.200", bars=2, arrows=("front",), unreadable=()):
    values = {
        "counter_glyph": "1",
        "primary_frequency": frequency,
        "active_bands": ["K"],
        "main_arrows": list(arrows),
        "main_bars": bars,
        "secondary": [],
        "muted_badge": False,
    }
    result = {name: {"state": "readable", "value": value} for name, value in values.items()}
    for name in unreadable:
        result[name] = {"state": "unreadable", "reason": "fixture glare"}
    return {"fields": result, "diagnostics": {"retained": True}}


def sequence_event(event_id, anchor, expected, *, end=None):
    return {"event_id": event_id, "start_ns": anchor - 20 * MS,
            "end_ns": anchor + 315 * MS if end is None else end,
            "end_reason": "camera_recording_ended",
            "target": expected,
            "target_basis": {"first_complete_target_input_ns": anchor,
                             "first_complete_target_stimulus_sequence": 7}}


def fixture(events, value_at=None, *, extra_offsets=(), dropped_offsets=()):
    previous_target = None
    for event in events:
        event.setdefault("mode", "BASELINE" if previous_target is None else
                         "UNCHANGED" if event["target"] == previous_target else "CHANGED")
        event.setdefault("previous_target", copy.deepcopy(previous_target))
        previous_target = event["target"]
    value_at = value_at or (lambda event, offset: observed(
        frequency=event["target"]["fields"]["primary_frequency"].get("allowed", ["24.200"])[0],
        bars=event["target"]["fields"]["main_bars"].get("allowed", [2])[0]))
    points = set()
    for event in events:
        anchor = event["target_basis"]["first_complete_target_input_ns"]
        stop = min(anchor + 312 * MS, event["end_ns"])
        points.update(range(anchor - 10 * MS, stop, 5 * MS))
    points.update(extra_offsets)
    dropped = set(dropped_offsets)
    records = []
    written_points = []
    for sequence_number, capture in enumerate(sorted(points | dropped), 1001):
        status = "writer_drop" if capture in dropped else "written"
        records.append({"status": status, "frame_seq": sequence_number,
                        "host_capture_ns": capture, "duration_ns": 5 * MS})
        if status == "written":
            written_points.append(capture)
    originals = []
    for video_index, capture in enumerate(written_points):
        matching = next((event for event in events
                         if event["target_basis"]["first_complete_target_input_ns"] - 10 * MS
                         <= capture < min(event["target_basis"]["first_complete_target_input_ns"]
                                          + 312 * MS, event["end_ns"])), None)
        if matching is None:
            continue
        anchor = matching["target_basis"]["first_complete_target_input_ns"]
        originals.append({"frame_id": f"source-{video_index:06d}",
                          "video_frame_index": video_index,
                          "source_frame_seq": records[next(
                              i for i, row in enumerate(records)
                              if row["status"] == "written"
                              and row["host_capture_ns"] == capture)]["frame_seq"],
                          "capture_ns": capture,
                          "image": f"frames/{video_index:06d}.png",
                          "observed": value_at(matching, capture - anchor)})
    return {"schema_version": 1, "events": events, "errors": []}, originals, records


def adapt(events, value_at=None, **kwargs):
    data = fixture(events, value_at, **kwargs)
    return adapter.adapt_sequence_events(*data), data


class EncounterProductAdapterTests(unittest.TestCase):
    def test_exact_half_open_window_and_original_evidence_are_preserved(self):
        event = sequence_event("event-0001", ANCHOR, target())
        outside = ANCHOR + 312 * MS
        result, data = adapt([event], extra_offsets=(outside,))
        product = result["events"][0]
        self.assertEqual(result["errors"], [])
        self.assertEqual(product["selection_window"],
                         {"start_ns": ANCHOR - 10 * MS, "end_ns": outside})
        self.assertEqual(product["observations"][0]["capture_ns"], ANCHOR - 10 * MS)
        self.assertEqual(product["observations"][-1]["capture_ns"], ANCHOR + 310 * MS)
        self.assertNotIn(outside, [item["capture_ns"] for item in product["observations"]])
        self.assertEqual(product["coverage"]["available_recorded_frames"], 65)
        self.assertEqual(product["coverage"]["first_source_boundary_gap_ns"], 0)
        self.assertEqual(product["coverage"]["last_source_boundary_gap_ns"], 2 * MS)
        self.assertEqual(product["required_joint_state_ids"], ["phase-1"])
        source = product["observations"][0]["source_observation_ref"]
        self.assertEqual(source, {key: data[1][0][key] for key in
                                  ("frame_id", "video_frame_index", "source_frame_seq",
                                   "capture_ns", "image")})

    def test_modes_and_previous_targets_are_preserved_from_the_full_sequence(self):
        first = sequence_event("event-0001", ANCHOR, target())
        same = sequence_event("event-0002", ANCHOR + 1_000 * MS, target())
        changed = sequence_event("event-0003", ANCHOR + 2_000 * MS, target(frequency="24.300"))
        result, _ = adapt([first, same, changed])
        self.assertEqual([event["mode"] for event in result["events"]],
                         ["BASELINE", "UNCHANGED", "CHANGED"])
        self.assertEqual([event["source_previous_target"] for event in result["events"]],
                         [None, first["target"], same["target"]])

    def test_legacy_response_claim_refuses_a_preexisting_target(self):
        prior = sequence_event("event-0001", ANCHOR, target(frequency="24.100"))
        current = sequence_event("event-0002", ANCHOR + 1_000 * MS,
                                 target(frequency="24.200"))
        sequence, originals, records = fixture([prior, current])
        selected = sequence["events"][1]
        anchor = selected["target_basis"]["first_complete_target_input_ns"]
        scoped_sequence = {**sequence, "events": [selected]}
        scoped_originals = [item for item in originals
                            if anchor - 10 * MS <= item["capture_ns"] < anchor + 312 * MS]

        adapted = adapter.adapt_sequence_events(scoped_sequence, scoped_originals, records)
        self.assertEqual(adapted["errors"], [])
        self.assertEqual(adapted["events"][0]["mode"], "CHANGED")
        self.assertEqual(adapted["events"][0]["source_previous_target"], prior["target"])
        self.assertTrue(all(item["raw_status"] == "CURRENT"
                            for item in adapted["events"][0]["observations"]))

        judged = judge_visible_event_presentation(
            adapted["events"], fatal_integrity_errors=adapted["errors"],
            policy_id="v1-normal-x-k-ka-blink96-v2")
        self.assertNotEqual(judged["result"], "PASS")
        self.assertEqual(judged["events"][0]["reason_code"], "TARGET_PREEXISTED")

    def test_supported_is_exactly_the_seven_resolved_target_fields(self):
        resolved = sequence_event("event-0001", ANCHOR, target())
        partial = sequence_event("event-0002", ANCHOR + 1_000 * MS,
                                 target(unresolved=("main_arrows",)))
        result, _ = adapt([resolved, partial])
        self.assertEqual([event["supported"] for event in result["events"]], [True, False])
        self.assertEqual(len(FIELDS), 7)

    def test_changed_event_maps_prior_full_match_then_current_with_state_id(self):
        prior = sequence_event("event-0001", ANCHOR, target(frequency="24.100"))
        current_anchor = ANCHOR + 1_000 * MS
        current = sequence_event("event-0002", current_anchor, target())

        def values(event, offset):
            if event is current and offset < 0:
                return observed(frequency="24.100")
            return observed(frequency=event["target"]["fields"]["primary_frequency"]["allowed"][0])

        result, _ = adapt([prior, current], values)
        observations = result["events"][1]["observations"]
        self.assertEqual([item["raw_status"] for item in observations[:2]],
                         ["PREVIOUS", "PREVIOUS"])
        first_current = next(item for item in observations if item["capture_ns"] >= current_anchor)
        self.assertEqual(first_current["raw_status"], "CURRENT")
        self.assertEqual(first_current["joint_state_id"], "phase-1")

    def test_overlap_between_changed_targets_is_conservatively_previous(self):
        prior_target = target(frequency="24.100")
        current_target = target(frequency="24.100")
        current_target["fields"]["primary_frequency"]["allowed"].append("24.200")
        prior = sequence_event("event-0001", ANCHOR, prior_target)
        current = sequence_event("event-0002", ANCHOR + 1_000 * MS, current_target)
        result, _ = adapt([prior, current], lambda event, offset: observed(frequency="24.100"))
        self.assertEqual(result["events"][1]["mode"], "CHANGED")
        self.assertTrue(all(item["raw_status"] == "PREVIOUS"
                            for item in result["events"][1]["observations"]))

    def test_readable_hybrid_is_definite_other_with_only_direct_fields(self):
        prior = sequence_event("event-0001", ANCHOR, target(frequency="24.100", bars=1))
        current = sequence_event("event-0002", ANCHOR + 1_000 * MS, target())

        def values(event, offset):
            if event is current:
                return observed(frequency="24.100", bars=2)
            return observed(frequency="24.100", bars=1)

        result, _ = adapt([prior, current], values)
        sample = result["events"][1]["observations"][0]
        self.assertEqual(sample["raw_status"], "DEFINITE_OTHER")
        self.assertEqual(sample["raw_affected_fields"], ["primary_frequency", "main_bars"])
        self.assertNotIn("joint_impossible", sample)

    def test_joint_state_is_reported_only_when_impossible_for_current_and_prior(self):
        prior = sequence_event("event-0001", ANCHOR, target(frequency="24.100"))
        current = sequence_event("event-0002", ANCHOR + 1_000 * MS, target())
        result, _ = adapt([prior, current], lambda event, offset: observed(
            frequency="24.150", arrows=("side",)))
        sample = result["events"][1]["observations"][0]
        self.assertEqual(sample["raw_status"], "DEFINITE_OTHER")
        self.assertEqual(sample["raw_affected_fields"],
                         ["primary_frequency", "main_arrows", "joint_state"])
        self.assertTrue(sample["joint_impossible"])

    def test_legal_prior_joint_is_never_called_an_impossible_joint(self):
        prior = sequence_event("event-0001", ANCHOR,
                               target(frequency="24.100", arrows=("rear",)))
        current = sequence_event("event-0002", ANCHOR + 1_000 * MS,
                                 target(frequency="24.200", arrows=("front",)))
        result, _ = adapt([prior, current], lambda event, offset: observed(
            frequency="24.200", arrows=("rear",)))
        sample = result["events"][1]["observations"][0]
        self.assertEqual(sample["raw_status"], "DEFINITE_OTHER")
        self.assertNotIn("joint_state", sample["raw_affected_fields"])
        self.assertNotIn("joint_impossible", sample)

    def test_unreadable_literal_stays_unresolved_and_names_only_that_field(self):
        event = sequence_event("event-0001", ANCHOR, target())
        result, data = adapt([event], lambda event, offset: observed(unreadable=("main_arrows",)))
        sample = result["events"][0]["observations"][0]
        self.assertEqual(sample["raw_status"], "UNRESOLVED")
        self.assertEqual(sample["raw_affected_fields"], ["main_arrows"])
        self.assertEqual(data[1][0]["observed"]["diagnostics"], {"retained": True})
        self.assertNotIn("observed", sample["source_observation_ref"])

    def test_temporal_arrow_qualification_cannot_hide_readable_frequency_regression(self):
        prior = sequence_event("event-0001", ANCHOR, target(frequency="24.100"))
        current_anchor = ANCHOR + 1_000 * MS
        current = sequence_event("event-0002", current_anchor, target(frequency="24.200"))

        def values(event, offset):
            if event is prior or (event is current and offset < 0):
                return observed(frequency="24.100")
            if event is current and offset == 50 * MS:
                return observed(frequency="24.100", unreadable=("main_arrows",))
            return observed(frequency="24.200")

        result, _ = adapt([prior, current], values)
        regressed = next(item for item in result["events"][1]["observations"]
                         if item["capture_ns"] == current_anchor + 50 * MS)
        self.assertEqual(regressed["raw_status"], "PREVIOUS")
        policy = load_policy("v1-normal-x-k-ka-blink96-v2")
        classifier_id = "fixture-arrow-phase-edge"
        classifier = {"classifier_spec_sha256": "a" * 64,
                      "raw_affected_fields": ["main_arrows"],
                      "deadline_observation_semantics": "LEGAL_PRESENTATION_TRANSITION"}
        policy["qualified_temporal_classifier_ids"] = [classifier_id]
        policy["qualified_temporal_classifiers"] = {classifier_id: classifier}
        arrow_claim = {
            "event_id": current["event_id"],
            "classifier_id": classifier_id,
            "classifier_spec_sha256": classifier["classifier_spec_sha256"],
            "status": "QUALIFIED_CAPTURE_TRANSITION",
            "video_frame_indices": [regressed["video_frame_index"]],
            "raw_affected_fields": ["main_arrows"],
        }
        with patch("encounter_product.load_policy", return_value=policy):
            judged = judge_visible_event_presentation(
                result["events"], temporal_classifications=[arrow_claim])
        self.assertEqual((judged["result"], judged["events"][1]["reason_code"]),
                         ("FAIL", "REGRESSION_AFTER_CURRENT"))
        self.assertTrue(judged["execution"]["temporal_classification_errors"])

    def test_unread_frame_remains_in_denominator_and_cannot_be_complete(self):
        event = sequence_event("event-0001", ANCHOR, target())
        result, data = adapt([event])
        del data[1][4]["observed"]
        result = adapter.adapt_sequence_events(*data)
        sample = result["events"][0]["observations"][4]
        self.assertEqual(sample["raw_status"], "UNRESOLVED")
        self.assertEqual(sample["raw_affected_fields"], list(FIELDS))
        self.assertEqual(result["events"][0]["coverage"]["read_recorded_frames"], 64)
        self.assertFalse(result["events"][0]["coverage"]["complete_recorded_frame_coverage"])

    def test_missing_extra_duplicate_or_wrong_identity_original_is_rejected(self):
        event = sequence_event("event-0001", ANCHOR, target())
        _, data = adapt([event])
        sequence, originals, records = data
        mutations = (
            lambda items: items.pop(),
            lambda items: items.insert(1, copy.deepcopy(items[0])),
            lambda items: items[0].update(capture_ns=items[0]["capture_ns"] + 1),
        )
        for mutate in mutations:
            with self.subTest(mutation=mutate):
                changed = copy.deepcopy(originals)
                mutate(changed)
                result = adapter.adapt_sequence_events(sequence, changed, records)
                self.assertEqual(result["events"], [])
                self.assertTrue(result["errors"])

        sequence, originals, records = fixture(
            [event], extra_offsets=(ANCHOR + 312 * MS,))
        outside_index = len([record for record in records if record["status"] == "written"]) - 1
        outside_row = [record for record in records if record["status"] == "written"][-1]
        originals.append({"frame_id": "outside-window", "video_frame_index": outside_index,
                          "source_frame_seq": outside_row["frame_seq"],
                          "capture_ns": outside_row["host_capture_ns"],
                          "observed": observed()})
        result = adapter.adapt_sequence_events(sequence, originals, records)
        self.assertEqual(result["events"], [])
        self.assertIn("exact written-frame union", result["errors"][0])

    def test_drop_and_source_sequence_gap_are_counted_from_the_sidecar(self):
        event = sequence_event("event-0001", ANCHOR, target())
        dropped = ANCHOR + 50 * MS
        result, _ = adapt([event], dropped_offsets=(dropped,))
        coverage = result["events"][0]["coverage"]
        self.assertEqual(coverage["unrecorded_source_frames"], 1)
        self.assertEqual(coverage["maximum_source_marker_gap_ns"], 10 * MS)
        self.assertEqual(len(coverage["source_sequence_discontinuities"]), 1)

    def test_clipped_event_keeps_full_declared_window_and_exact_clipped_coverage(self):
        event = sequence_event("event-0001", ANCHOR, target(), end=ANCHOR + 250 * MS)
        result, _ = adapt([event])
        product = result["events"][0]
        self.assertEqual(product["end_ns"], ANCHOR + 250 * MS)
        self.assertEqual(product["selection_window"]["end_ns"], ANCHOR + 312 * MS)
        self.assertTrue(all(item["capture_ns"] < product["end_ns"]
                            for item in product["observations"]))

    def test_missing_compare_state_id_is_never_reconstructed_by_the_adapter(self):
        event = sequence_event("event-0001", ANCHOR, target())

        def no_state_id(expected, reading, role):
            return {"checks": {name: {"status": "MATCH"} for name in FIELDS},
                    "joint_state": {"status": "MATCH"}}

        with patch.object(adapter, "compare_sample", side_effect=no_state_id):
            result, _ = adapt([event])
        sample = result["events"][0]["observations"][0]
        self.assertEqual(sample["raw_status"], "UNRESOLVED")
        self.assertNotIn("joint_state_id", sample)

    def test_second_joint_phase_uses_compare_samples_stable_phase_2_id(self):
        expected = target()
        expected["fields"]["main_arrows"]["allowed"].append(["rear"])
        expected["joint_states"].append(
            {"counter_glyph": "1", "active_bands": ["K"], "main_arrows": ["rear"]})
        event = sequence_event("event-0001", ANCHOR, expected)
        result, _ = adapt([event], lambda event, offset: observed(arrows=("rear",)))
        product = result["events"][0]
        self.assertEqual(product["required_joint_state_ids"], ["phase-1", "phase-2"])
        self.assertTrue(all(item["joint_state_id"] == "phase-2"
                            for item in product["observations"]))

    def test_inputs_are_side_effect_free(self):
        event = sequence_event("event-0001", ANCHOR, target())
        data = fixture([event])
        frozen = copy.deepcopy(data)
        adapter.adapt_sequence_events(*data)
        self.assertEqual(data, frozen)

    def test_adapter_output_is_accepted_directly_by_the_product_judge(self):
        event = sequence_event("event-0001", ANCHOR, target())
        adapted, _ = adapt([event])
        judged = judge_visible_event_presentation(
            adapted["events"], fatal_integrity_errors=adapted["errors"])
        self.assertEqual(judged["result"], "PASS")
        self.assertEqual(judged["events"][0]["raw_status_counts"], {"CURRENT": 65})


if __name__ == "__main__":
    unittest.main()
