#!/usr/bin/env python3
"""Adversarial contract tests for the external visible-event product judgment."""

from contextlib import contextmanager
import copy
import json
from pathlib import Path
import sys
import tempfile
import unittest


sys.path.insert(0, str(Path(__file__).resolve().parent / "bench"))
from encounter_product import (DEFAULT_POLICY_PATH,
                               judge_visible_event_presentation as judge_current_contract,
                               load_policy as load_current_policy)

# Preserve the historical acquisition/coherence contract regressions explicitly.
DEFAULT_POLICY_ID = "v1-normal-x-k-ka-blink96-v2"

def judge_visible_event_presentation(*args, **kwargs):
    kwargs.setdefault("policy_id", DEFAULT_POLICY_ID)
    return judge_current_contract(*args, **kwargs)

def load_policy(policy_id=DEFAULT_POLICY_ID, path=DEFAULT_POLICY_PATH):
    return load_current_policy(policy_id, path)


MS = 1_000_000
ANCHOR = 1_000_000_000
TEST_CLASSIFIER = "synthetic-qualified-transition-v1"
TEST_CLASSIFIER_SPEC_SHA256 = "1" * 64


def refresh_coverage(event):
    observations = event["observations"]
    count = len(observations)
    gaps = [right["capture_ns"] - left["capture_ns"]
            for left, right in zip(observations, observations[1:])]
    event["coverage"] = {
        "available_recorded_frames": count,
        "selected_recorded_frames": count,
        "read_recorded_frames": count,
        "unrecorded_source_frames": 0,
        "maximum_source_marker_gap_ns": max(gaps, default=0),
        "complete_recorded_frame_coverage": True,
    }
    return event


def make_event(*, event_id="event-0001", mode="CHANGED", first_current_ns=100 * MS,
               phases=("phase-a",), alternate_phases=False, supported=True,
               marker_phase_ns=0):
    offsets = list(range(-5 * MS + marker_phase_ns, 312 * MS, 5 * MS))
    if 0 <= first_current_ns <= 300 * MS:
        offsets.append(first_current_ns)
    offsets = sorted(set(offsets))
    observations = []
    for index, offset in enumerate(offsets):
        if offset < 0:
            status = "CURRENT" if mode == "UNCHANGED" else "PREVIOUS"
        else:
            status = "CURRENT" if offset >= first_current_ns else "PREVIOUS"
        item = {
            "frame_id": f"frame-{index:04d}",
            "video_frame_index": 100 + index,
            "source_frame_seq": 1001 + index,
            "capture_ns": ANCHOR + offset,
            "raw_status": status,
            "raw_affected_fields": [],
        }
        if status == "CURRENT":
            phase = phases[(offset // (96 * MS)) % len(phases)] if alternate_phases else phases[0]
            item["joint_state_id"] = phase
        observations.append(item)
    event = {
        "event_id": event_id,
        "mode": mode,
        "supported": supported,
        "target_basis": {"first_complete_target_input_ns": ANCHOR,
                         "first_complete_target_stimulus_sequence": 7},
        "end_ns": ANCHOR + 312 * MS,
        "end_reason": "next_changed_input_requested",
        "selection_window": {"start_ns": ANCHOR - 10 * MS,
                             "end_ns": ANCHOR + 312 * MS},
        "required_joint_state_ids": list(phases),
        "observations": observations,
    }
    return refresh_coverage(event)


def observation_at(event, offset_ns):
    return next(item for item in event["observations"] if item["capture_ns"] == ANCHOR + offset_ns)


def set_observation(event, offset_ns, status, *, fields=(), joint_impossible=False, phase="phase-a"):
    item = observation_at(event, offset_ns)
    item["raw_status"] = status
    item["raw_affected_fields"] = list(fields)
    item.pop("joint_state_id", None)
    item.pop("joint_impossible", None)
    if status == "CURRENT":
        item["joint_state_id"] = phase
    if status == "DEFINITE_OTHER" and joint_impossible:
        item["joint_impossible"] = True
    return item


def set_range(event, start_ns, status, *, fields=(), end_ns=312 * MS, phase="phase-a"):
    selected = []
    for item in event["observations"]:
        offset = item["capture_ns"] - ANCHOR
        if start_ns <= offset < end_ns:
            selected.append(item["video_frame_index"])
            item["raw_status"] = status
            item["raw_affected_fields"] = list(fields)
            item.pop("joint_state_id", None)
            item.pop("joint_impossible", None)
            if status == "CURRENT":
                item["joint_state_id"] = phase
    return selected


def qualification(event, indices, *, classifier_id=TEST_CLASSIFIER, fields=("main_arrows",)):
    return {
        "event_id": event["event_id"],
        "classifier_id": classifier_id,
        "classifier_spec_sha256": TEST_CLASSIFIER_SPEC_SHA256,
        "status": "QUALIFIED_CAPTURE_TRANSITION",
        "video_frame_indices": sorted(indices),
        "raw_affected_fields": list(fields),
    }


@contextmanager
def policy_allowing(*classifier_ids, fields_by_classifier=None,
                    semantics_by_classifier=None, closure_by_classifier=None, auxiliary_by_classifier=None,
                    policy_id=DEFAULT_POLICY_ID):
    fields_by_classifier = fields_by_classifier or {}
    semantics_by_classifier = semantics_by_classifier or {}
    closure_by_classifier = closure_by_classifier or {}
    auxiliary_by_classifier = auxiliary_by_classifier or {}
    document = json.loads(DEFAULT_POLICY_PATH.read_text(encoding="utf-8"))
    document["policies"][policy_id][
        "qualified_temporal_classifier_ids"] = list(classifier_ids)
    entries = {}
    for classifier_id in classifier_ids:
        entries[classifier_id] = {
            "classifier_spec_sha256": TEST_CLASSIFIER_SPEC_SHA256,
            "deadline_observation_semantics": semantics_by_classifier.get(
                classifier_id, "LEGAL_PRESENTATION_TRANSITION"),
            "raw_affected_fields": list(
                fields_by_classifier.get(classifier_id, ("main_arrows",))),
        }
        if classifier_id in auxiliary_by_classifier:
            entries[classifier_id]["auxiliary_closure_context_ns"] = auxiliary_by_classifier[classifier_id]
        if classifier_id in closure_by_classifier:
            entries[classifier_id]["verification_closure_semantics"] = (
                closure_by_classifier[classifier_id])
    document["policies"][policy_id]["qualified_temporal_classifiers"] = entries
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "policies.json"
        path.write_text(json.dumps(document), encoding="utf-8")
        yield path


class VisibleEventProductTests(unittest.TestCase):
    def test_01_policy_is_versioned_and_derived_from_declared_product_clocks(self):
        policy = load_policy()
        self.assertEqual(policy["contract_id"], "VISIBLE_EVENT_PRESENTATION")
        self.assertEqual(policy["contract_version"], 2)
        self.assertEqual(policy["appearance_decision_rule"],
                         "first_source_marker_at_or_after_nominal_deadline")
        self.assertEqual(policy["appearance_deadline_ns"], 100 * MS)
        self.assertEqual(policy["maximum_appearance_observation_bracket_ns"], 10 * MS)
        self.assertEqual(policy["verification_duration_ns"], 192 * MS)
        self.assertEqual(policy["maximum_source_marker_gap_ns"], 10 * MS)
        self.assertEqual(policy["minimum_post_completion_hold_ns"], 312 * MS)
        self.assertEqual(set(policy["qualified_temporal_classifier_ids"]),
                         set(policy["qualified_temporal_classifiers"]))
        self.assertTrue(all(len(spec["classifier_spec_sha256"]) == 64
                            for spec in policy["qualified_temporal_classifiers"].values()))
        self.assertTrue(all(spec["deadline_observation_semantics"] ==
                            "LEGAL_PRESENTATION_TRANSITION"
                            for spec in policy["qualified_temporal_classifiers"].values()))

    def test_02_target_at_inclusive_deadline_and_full_verification_passes(self):
        result = judge_visible_event_presentation([make_event()])
        self.assertEqual(result["result"], "PASS")
        self.assertEqual(result["events"][0]["first_current_correct"]["capture_ns"], ANCHOR + 100 * MS)
        self.assertGreaterEqual(result["events"][0]["closing_current_correct"]["capture_ns"],
                                ANCHOR + 292 * MS)

    def test_02a_baseline_current_at_first_postdeadline_marker_uses_one_bounded_observation(self):
        phase = 74_208
        event = make_event(mode="BASELINE", first_current_ns=100 * MS + phase,
                           marker_phase_ns=phase)
        set_range(event, 0, "UNRESOLVED", fields=("counter_glyph",), end_ns=40 * MS)
        set_range(event, 40 * MS, "DEFINITE_OTHER", fields=("counter_glyph",),
                  end_ns=90 * MS)
        set_range(event, 90 * MS, "UNRESOLVED", fields=("counter_glyph",),
                  end_ns=100 * MS + phase)

        result = judge_visible_event_presentation([event])
        judged = result["events"][0]
        self.assertEqual((result["result"], judged["reason_code"]),
                         ("PASS", "CURRENT_AT_FIRST_POST_DEADLINE_CAPTURE_AND_VERIFIED"))
        self.assertEqual(judged["first_current_correct"]["capture_ns"],
                         ANCHOR + 100 * MS + phase)
        acquisition = judged["response_acquisition"]
        self.assertEqual(acquisition["status"], "CURRENT_AT_FIRST_POST_DEADLINE_CAPTURE")
        self.assertEqual(acquisition["first_current_offset_from_nominal_deadline_ns"], phase)
        self.assertEqual(acquisition["deadline_capture_marker_bracket"]["start"]["capture_ns"],
                         ANCHOR + 95 * MS + phase)
        self.assertEqual(acquisition["deadline_capture_marker_bracket"]["end"]["capture_ns"],
                         ANCHOR + 100 * MS + phase)
        self.assertEqual(acquisition["deadline_capture_marker_bracket"]["width_ns"], 5 * MS)
        self.assertTrue(acquisition["deadline_capture_marker_bracket"]["within_limit"])

    def test_02b_postdeadline_current_cannot_pass_with_an_oversized_marker_bracket(self):
        phase = 1
        event = make_event(mode="BASELINE", first_current_ns=100 * MS + phase,
                           marker_phase_ns=phase)
        event["observations"] = [item for item in event["observations"]
                                 if item["capture_ns"] not in {
                                     ANCHOR + 90 * MS + phase,
                                     ANCHOR + 95 * MS + phase,
                                 }]
        refresh_coverage(event)

        result = judge_visible_event_presentation([event])
        judged = result["events"][0]
        self.assertEqual(result["result"], "INCONCLUSIVE")
        self.assertIn("DEADLINE_OBSERVATION_BRACKET_NOT_ESTABLISHED", judged["reasons"])
        self.assertFalse(judged["response_acquisition"][
            "deadline_capture_marker_bracket"]["within_limit"])

    def test_03_first_deadline_marker_previous_fails_and_later_current_gets_no_grace(self):
        result = judge_visible_event_presentation([make_event(first_current_ns=100 * MS + 1)])
        self.assertEqual((result["result"], result["reason_code"]),
                         ("FAIL", "PREVIOUS_STATE_AFTER_DEADLINE"))

    def test_04_previous_state_after_deadline_is_a_supported_failure(self):
        result = judge_visible_event_presentation([make_event(first_current_ns=110 * MS)])
        self.assertEqual((result["result"], result["reason_code"]),
                         ("FAIL", "PREVIOUS_STATE_AFTER_DEADLINE"))

    def test_05_first_postdeadline_unresolved_is_inconclusive_even_when_temporally_qualified(self):
        event = make_event(first_current_ns=400 * MS)
        indices = set_range(event, 100 * MS, "UNRESOLVED", fields=("main_arrows",))
        record = qualification(event, indices)
        with policy_allowing(TEST_CLASSIFIER) as policy_path:
            result = judge_visible_event_presentation([event], temporal_classifications=[record],
                                                       policy_path=policy_path)
        self.assertEqual((result["result"], result["reason_code"]),
                         ("INCONCLUSIVE", "DEADLINE_OBSERVATION_UNRESOLVED"))
        self.assertEqual(result["events"][0]["response_acquisition"]["status"],
                         "FIRST_POST_DEADLINE_CAPTURE_UNRESOLVED")

    def test_05a_qualified_target_acquisition_transition_at_deadline_fails(self):
        event = make_event(first_current_ns=105 * MS)
        frame = set_observation(event, 100 * MS, "UNRESOLVED", fields=("main_bars",))
        record = qualification(event, [frame["video_frame_index"]], fields=("main_bars",))
        with policy_allowing(
                TEST_CLASSIFIER, fields_by_classifier={TEST_CLASSIFIER: ("main_bars",)},
                semantics_by_classifier={
                    TEST_CLASSIFIER: "TARGET_ACQUISITION_TRANSITION"}) as policy_path:
            result = judge_visible_event_presentation(
                [event], temporal_classifications=[record], policy_path=policy_path)
        judged = result["events"][0]
        self.assertEqual((result["result"], result["reason_code"]),
                         ("FAIL", "TARGET_IN_TRANSITION_AT_DEADLINE_CAPTURE"))
        self.assertEqual(judged["response_acquisition"]["status"],
                         "FIRST_POST_DEADLINE_CAPTURE_TARGET_IN_TRANSITION")

    def test_06_unresolved_first_deadline_marker_gives_no_grace_to_later_current(self):
        event = make_event(first_current_ns=105 * MS)
        set_observation(event, 100 * MS, "UNRESOLVED", fields=("main_arrows",))
        result = judge_visible_event_presentation([event])
        self.assertEqual((result["result"], result["reason_code"]),
                         ("INCONCLUSIVE", "DEADLINE_OBSERVATION_UNRESOLVED"))
        self.assertEqual(result["events"][0]["first_current_correct"]["capture_ns"],
                         ANCHOR + 105 * MS)
        self.assertEqual(result["events"][0]["response_acquisition"]["status"],
                         "CURRENT_ONLY_AFTER_FIRST_POST_DEADLINE_CAPTURE")

    def test_07_invented_third_state_fails_even_before_the_target_arrives(self):
        event = make_event()
        set_observation(event, 20 * MS, "DEFINITE_OTHER", fields=("primary_frequency",))
        result = judge_visible_event_presentation([event])
        self.assertEqual((result["result"], result["reason_code"]),
                         ("FAIL", "UNEXPECTED_VISIBLE_STATE"))

    def test_07a_baseline_non_current_prefix_can_reach_and_verify_target(self):
        event = make_event(mode="BASELINE", first_current_ns=50 * MS)
        set_range(event, 0, "DEFINITE_OTHER", fields=("primary_frequency",),
                  end_ns=50 * MS)
        result = judge_visible_event_presentation([event])
        self.assertEqual((result["result"], result["events"][0]["reason_code"]),
                         ("PASS", "PRESENTED_BY_DEADLINE_AND_VERIFIED"))
        self.assertEqual(result["events"][0]["first_current_correct"]["capture_ns"],
                         ANCHOR + 50 * MS)

    def test_07b_baseline_non_current_after_deadline_or_current_is_a_failure(self):
        late = make_event(mode="BASELINE", first_current_ns=110 * MS)
        set_range(late, 0, "DEFINITE_OTHER", fields=("primary_frequency",),
                  end_ns=110 * MS)
        late_result = judge_visible_event_presentation([late])
        self.assertEqual((late_result["result"], late_result["events"][0]["reason_code"]),
                         ("FAIL", "UNEXPECTED_VISIBLE_STATE"))

        regressed = make_event(mode="BASELINE", first_current_ns=20 * MS)
        set_observation(regressed, 40 * MS, "DEFINITE_OTHER", fields=("primary_frequency",))
        regression_result = judge_visible_event_presentation([regressed])
        self.assertEqual((regression_result["result"],
                          regression_result["events"][0]["reason_code"]),
                         ("FAIL", "UNEXPECTED_VISIBLE_STATE"))

    def test_08_readable_mixture_of_previous_and_current_fields_is_not_transition_tolerance(self):
        event = make_event()
        set_observation(event, 40 * MS, "DEFINITE_OTHER",
                        fields=("primary_frequency", "main_arrows"))
        result = judge_visible_event_presentation([event])
        self.assertEqual(result["events"][0]["reason_code"], "UNEXPECTED_VISIBLE_STATE")

    def test_09_previous_state_reappearing_after_current_is_a_regression(self):
        event = make_event(first_current_ns=50 * MS)
        set_observation(event, 80 * MS, "PREVIOUS", fields=("primary_frequency",))
        result = judge_visible_event_presentation([event])
        self.assertEqual((result["result"], result["reason_code"]),
                         ("FAIL", "REGRESSION_AFTER_CURRENT"))

    def test_10_correct_wrong_correct_fails_without_majority_vote(self):
        event = make_event(first_current_ns=20 * MS)
        # 215 ms is the first source marker after the 212 ms verification end.
        # A later correct frame cannot replace this closing-boundary failure.
        set_observation(event, 215 * MS, "DEFINITE_OTHER", fields=("main_bars",))
        result = judge_visible_event_presentation([event])
        self.assertEqual(result["result"], "FAIL")
        self.assertEqual(result["events"][0]["first_decisive_marker"]["capture_ns"], ANCHOR + 215 * MS)

    def test_11_correct_generic_unknown_correct_remains_inconclusive(self):
        event = make_event(first_current_ns=20 * MS)
        set_observation(event, 100 * MS, "UNRESOLVED", fields=("main_arrows",))
        result = judge_visible_event_presentation([event])
        self.assertEqual((result["result"], result["reason_code"]),
                         ("INCONCLUSIVE", "UNCLASSIFIED_VISIBLE_INTERVAL"))

    def test_12_explicit_policy_listed_transition_can_pass_without_rewriting_raw_status(self):
        event = make_event(first_current_ns=20 * MS)
        indices = [set_observation(event, offset, "UNRESOLVED", fields=("main_arrows",))[
            "video_frame_index"] for offset in (95 * MS, 100 * MS)]
        record = qualification(event, indices)
        original = copy.deepcopy((event, [record]))
        with policy_allowing(TEST_CLASSIFIER) as policy_path:
            result = judge_visible_event_presentation([event], temporal_classifications=[record],
                                                       policy_path=policy_path)
        judged = result["events"][0]
        self.assertEqual(result["result"], "PASS")
        self.assertEqual([item["raw_status"] for item in judged["raw_observations"]
                          if item["video_frame_index"] in indices], ["UNRESOLVED", "UNRESOLVED"])
        self.assertEqual({item["derived_status"] for item in judged["derived_observations"]
                          if item["video_frame_index"] in indices}, {"QUALIFIED_CAPTURE_TRANSITION"})
        self.assertEqual((event, [record]), original)

    def test_13_one_exact_current_marker_does_not_establish_verification_duration(self):
        event = make_event(first_current_ns=100 * MS)
        indices = set_range(event, 105 * MS, "UNRESOLVED", fields=("main_arrows",))
        record = qualification(event, indices)
        with policy_allowing(TEST_CLASSIFIER) as policy_path:
            result = judge_visible_event_presentation([event], temporal_classifications=[record],
                                                       policy_path=policy_path)
        self.assertEqual((result["result"], result["events"][0]["reason_code"]),
                         ("INCONCLUSIVE", "VERIFICATION_DURATION_NOT_OBSERVED"))

    def test_14_source_gap_or_duplicate_cannot_supply_recurrence(self):
        event = make_event()
        event["observations"] = [item for item in event["observations"]
                                 if item["capture_ns"] != ANCHOR + 50 * MS]
        refresh_coverage(event)
        result = judge_visible_event_presentation([event])
        self.assertEqual(result["result"], "INCONCLUSIVE")
        self.assertIn("SOURCE_MARKER_GAP", result["events"][0]["reasons"])

    def test_15_definite_failure_survives_a_source_gap(self):
        event = make_event()
        set_observation(event, 20 * MS, "DEFINITE_OTHER", fields=("main_bars",))
        event["observations"] = [item for item in event["observations"]
                                 if item["capture_ns"] != ANCHOR + 50 * MS]
        refresh_coverage(event)
        result = judge_visible_event_presentation([event])
        self.assertEqual((result["result"], result["reason_code"]),
                         ("FAIL", "UNEXPECTED_VISIBLE_STATE"))

    def test_16_blink_requires_both_joint_phases_and_a_full_cycle(self):
        passing = make_event(first_current_ns=20 * MS, phases=("image-1", "image-2"),
                             alternate_phases=True)
        missing = make_event(first_current_ns=20 * MS, phases=("image-1", "image-2"))
        passed = judge_visible_event_presentation([passing])
        blocked = judge_visible_event_presentation([missing])
        self.assertEqual(passed["result"], "PASS")
        self.assertEqual(blocked["result"], "INCONCLUSIVE")
        self.assertIn("LEGAL_BLINK_PHASE_NOT_OBSERVED", blocked["events"][0]["reasons"])

    def test_17_impossible_joint_state_is_a_failure(self):
        event = make_event()
        set_observation(event, 30 * MS, "DEFINITE_OTHER", fields=("joint_state",),
                        joint_impossible=True)
        result = judge_visible_event_presentation([event])
        self.assertEqual((result["result"], result["reason_code"]),
                         ("FAIL", "IMPOSSIBLE_JOINT_STATE"))

    def test_18_changed_target_already_visible_before_input_has_no_response_claim(self):
        event = make_event()
        set_observation(event, -5 * MS, "CURRENT", phase="phase-a")
        result = judge_visible_event_presentation([event])
        self.assertEqual(result["result"], "INCONCLUSIVE")
        self.assertEqual(result["events"][0]["reason_code"], "TARGET_PREEXISTED")

    def test_19_unchanged_target_is_verified_without_claiming_a_visible_change(self):
        event = make_event(mode="UNCHANGED", first_current_ns=0)
        result = judge_visible_event_presentation([event])
        self.assertEqual(result["result"], "PASS")
        self.assertEqual(result["events"][0]["mode"], "UNCHANGED")

    def test_20_stateful_mute_uses_the_second_accepted_display_as_its_anchor(self):
        second_accept = ANCHOR
        event = make_event(mode="BASELINE", first_current_ns=0)
        event["target_basis"].update(first_mute_display_accepted_ns=ANCHOR - 333 * MS,
                                     second_mute_display_accepted_ns=second_accept)
        result = judge_visible_event_presentation([event])
        self.assertEqual(result["result"], "PASS")
        self.assertEqual(result["events"][0]["window"]["anchor_ns"], second_accept)

    def test_21_superseding_or_unscoped_input_before_312ms_clips_the_episode(self):
        event = make_event(first_current_ns=20 * MS)
        event["end_ns"] = ANCHOR + 250 * MS
        event["end_reason"] = "unscoped_input_requested"
        event["observations"] = [item for item in event["observations"]
                                 if item["capture_ns"] < event["end_ns"]]
        refresh_coverage(event)
        result = judge_visible_event_presentation([event])
        self.assertEqual(result["result"], "INCONCLUSIVE")
        self.assertEqual(result["events"][0]["reason_code"], "EPISODE_CLIPPED")

    def test_22_fatal_identity_or_reader_qualification_error_overrides_pixel_claims(self):
        event = make_event()
        set_observation(event, 20 * MS, "DEFINITE_OTHER", fields=("main_bars",))
        result = judge_visible_event_presentation(
            [event], fatal_integrity_errors=["reader qualification bundle hash mismatch"])
        self.assertEqual((result["result"], result["reason_code"]),
                         ("INCONCLUSIVE", "FATAL_EVIDENCE_INTEGRITY"))
        self.assertEqual(result["events"], [])

    def test_23_field_failure_beside_raw_unknown_and_bad_classifier_claim_still_fails(self):
        event = make_event()
        wrong = set_observation(event, 20 * MS, "DEFINITE_OTHER",
                                fields=("main_bars", "main_arrows"))
        record = qualification(event, [wrong["video_frame_index"]],
                               classifier_id="not-policy-listed", fields=("main_arrows",))
        result = judge_visible_event_presentation([event], temporal_classifications=[record])
        self.assertEqual((result["result"], result["reason_code"]),
                         ("FAIL", "UNEXPECTED_VISIBLE_STATE"))
        self.assertTrue(result["execution"]["temporal_classification_errors"])

    def test_24_unsupported_scope_or_tampered_window_can_never_pass(self):
        unsupported = judge_visible_event_presentation([make_event(supported=False)])
        self.assertEqual((unsupported["result"], unsupported["reason_code"]),
                         ("INCONCLUSIVE", "UNSUPPORTED_TARGET"))
        tampered = make_event()
        tampered["selection_window"]["end_ns"] += 1
        rejected = judge_visible_event_presentation([tampered])
        self.assertEqual(rejected["result"], "INCONCLUSIVE")
        self.assertEqual(rejected["events"][0]["reason_code"], "INVALID_EVENT_EVIDENCE")

    def test_25_disjoint_classifiers_compose_to_cover_one_raw_ambiguous_frame(self):
        event = make_event(first_current_ns=20 * MS)
        frame = set_observation(event, 100 * MS, "UNRESOLVED",
                                fields=("main_arrows", "main_bars"))
        arrows_classifier = "synthetic-arrows-transition-v1"
        bars_classifier = "synthetic-bars-transition-v1"
        records = [
            qualification(event, [frame["video_frame_index"]],
                          classifier_id=arrows_classifier, fields=("main_arrows",)),
            qualification(event, [frame["video_frame_index"]],
                          classifier_id=bars_classifier, fields=("main_bars",)),
        ]
        with policy_allowing(
                arrows_classifier, bars_classifier,
                fields_by_classifier={arrows_classifier: ("main_arrows",),
                                      bars_classifier: ("main_bars",)}) as policy_path:
            result = judge_visible_event_presentation(
                [event], temporal_classifications=records, policy_path=policy_path)
        judged = result["events"][0]
        derived = next(item for item in judged["derived_observations"]
                       if item["video_frame_index"] == frame["video_frame_index"])
        self.assertEqual(result["result"], "PASS")
        self.assertEqual(derived["derived_status"], "QUALIFIED_CAPTURE_TRANSITION")
        self.assertEqual({item["classifier_id"] for item in judged["temporal_classifications"]},
                         {arrows_classifier, bars_classifier})
        self.assertEqual(result["execution"]["temporal_classification_errors"], [])

    def test_26_duplicate_field_claims_reject_every_implicated_record(self):
        event = make_event(first_current_ns=20 * MS)
        frame = set_observation(event, 100 * MS, "UNRESOLVED", fields=("main_arrows",))
        first_classifier = "synthetic-arrows-first-v1"
        second_classifier = "synthetic-arrows-second-v1"
        records = [
            qualification(event, [frame["video_frame_index"]], classifier_id=first_classifier),
            qualification(event, [frame["video_frame_index"]], classifier_id=second_classifier),
        ]
        with policy_allowing(first_classifier, second_classifier) as policy_path:
            result = judge_visible_event_presentation(
                [event], temporal_classifications=records, policy_path=policy_path)
        errors = result["execution"]["temporal_classification_errors"]
        self.assertEqual((result["result"], result["events"][0]["reason_code"]),
                         ("INCONCLUSIVE", "UNCLASSIFIED_VISIBLE_INTERVAL"))
        self.assertEqual([item["code"] for item in errors],
                         ["DUPLICATE_TEMPORAL_CLASSIFICATION_FIELD_CLAIM"] * 2)
        self.assertEqual(result["events"][0]["temporal_classifications"], [])

    def test_27_partial_field_union_does_not_qualify_the_raw_frame(self):
        event = make_event(first_current_ns=20 * MS)
        frame = set_observation(event, 100 * MS, "UNRESOLVED",
                                fields=("main_arrows", "main_bars"))
        record = qualification(event, [frame["video_frame_index"]], fields=("main_arrows",))
        with policy_allowing(TEST_CLASSIFIER) as policy_path:
            result = judge_visible_event_presentation(
                [event], temporal_classifications=[record], policy_path=policy_path)
        self.assertEqual((result["result"], result["events"][0]["reason_code"]),
                         ("INCONCLUSIVE", "UNCLASSIFIED_VISIBLE_INTERVAL"))
        self.assertEqual(result["execution"]["temporal_classification_errors"], [])
        self.assertEqual(result["events"][0]["temporal_classifications"], [])

    def test_27a_partial_target_acquisition_evidence_proves_deadline_failure(self):
        event = make_event(first_current_ns=105 * MS)
        frame = set_observation(event, 100 * MS, "UNRESOLVED",
                                fields=("main_arrows", "main_bars"))
        record = qualification(event, [frame["video_frame_index"]], fields=("main_arrows",))
        with policy_allowing(
                TEST_CLASSIFIER,
                fields_by_classifier={TEST_CLASSIFIER: ("main_arrows",)},
                semantics_by_classifier={
                    TEST_CLASSIFIER: "TARGET_ACQUISITION_TRANSITION"}) as policy_path:
            result = judge_visible_event_presentation(
                [event], temporal_classifications=[record], policy_path=policy_path)
        judged = result["events"][0]
        self.assertEqual((result["result"], judged["reason_code"]),
                         ("FAIL", "TARGET_IN_TRANSITION_AT_DEADLINE_CAPTURE"))
        self.assertEqual(judged["derived_observations"][-1]["derived_status"], "UNRESOLVED")
        self.assertEqual(judged["temporal_classifications"], [record])

    def test_27b_partial_target_acquisition_evidence_cannot_excuse_verification(self):
        event = make_event(first_current_ns=20 * MS)
        frame = set_observation(event, 105 * MS, "UNRESOLVED",
                                fields=("main_arrows", "main_bars"))
        record = qualification(event, [frame["video_frame_index"]], fields=("main_arrows",))
        with policy_allowing(
                TEST_CLASSIFIER,
                fields_by_classifier={TEST_CLASSIFIER: ("main_arrows",)},
                semantics_by_classifier={
                    TEST_CLASSIFIER: "TARGET_ACQUISITION_TRANSITION"}) as policy_path:
            result = judge_visible_event_presentation(
                [event], temporal_classifications=[record], policy_path=policy_path)
        judged = result["events"][0]
        self.assertEqual((result["result"], judged["reason_code"]),
                         ("INCONCLUSIVE", "UNCLASSIFIED_VISIBLE_INTERVAL"))
        self.assertEqual(judged["temporal_classifications"], [])

    def test_27c_exact_target_acquisition_evidence_cannot_excuse_verification(self):
        event = make_event(first_current_ns=20 * MS)
        frame = set_observation(event, 105 * MS, "UNRESOLVED", fields=("main_arrows",))
        record = qualification(event, [frame["video_frame_index"]], fields=("main_arrows",))
        with policy_allowing(
                TEST_CLASSIFIER,
                fields_by_classifier={TEST_CLASSIFIER: ("main_arrows",)},
                semantics_by_classifier={
                    TEST_CLASSIFIER: "TARGET_ACQUISITION_TRANSITION"}) as policy_path:
            result = judge_visible_event_presentation(
                [event], temporal_classifications=[record], policy_path=policy_path)
        judged = result["events"][0]
        self.assertEqual((result["result"], judged["reason_code"]),
                         ("INCONCLUSIVE", "UNCLASSIFIED_VISIBLE_INTERVAL"))
        self.assertEqual(judged["temporal_classifications"], [])

    def test_27d_functional_contract_requires_raw_current_closing_marker(self):
        event = make_event(first_current_ns=20 * MS)
        indices = set_range(event, 295 * MS, "UNRESOLVED", fields=("primary_frequency",))
        record = qualification(event, indices, fields=("primary_frequency",))
        policy_id = "v1-normal-x-k-ka-blink96-v3"
        with policy_allowing(
                TEST_CLASSIFIER,
                fields_by_classifier={TEST_CLASSIFIER: ("primary_frequency",)},
                policy_id=policy_id) as policy_path:
            result = judge_current_contract(
                [event], temporal_classifications=[record], policy_id=policy_id,
                policy_path=policy_path)
        judged = result["events"][0]
        self.assertEqual((result["result"], judged["reason_code"]),
                         ("INCONCLUSIVE", "VERIFICATION_DURATION_NOT_OBSERVED"))
        self.assertIsNone(judged["closing_current_correct"])
        self.assertEqual(judged["temporal_classifications"], [record])

    def test_27e_closed_exact_current_run_can_cross_the_verification_boundary(self):
        event = make_event(first_current_ns=20 * MS)
        run = [set_observation(event, offset * MS, "UNRESOLVED",
                               fields=("primary_frequency",))
               for offset in (295, 300)]
        record = qualification(
            event, [point["video_frame_index"] for point in run],
            fields=("primary_frequency",))
        record.update(
            verification_closure_semantics=(
                "RAW_CURRENT_BRACKETED_UNRESOLVED_VERIFICATION_BOUNDARY"),
            context_frame_indices=[observation_at(event, offset * MS)["video_frame_index"]
                                   for offset in (290, 295, 300, 305)],
            first={key: run[0][key] for key in (
                "frame_id", "video_frame_index", "source_frame_seq", "capture_ns")},
            last={key: run[-1][key] for key in (
                "frame_id", "video_frame_index", "source_frame_seq", "capture_ns")},
        )
        policy_id = "v1-normal-x-k-ka-blink96-v3"
        with policy_allowing(
                TEST_CLASSIFIER,
                fields_by_classifier={TEST_CLASSIFIER: ("primary_frequency",)},
                closure_by_classifier={TEST_CLASSIFIER:
                    "RAW_CURRENT_BRACKETED_UNRESOLVED_VERIFICATION_BOUNDARY"},
                policy_id=policy_id) as policy_path:
            result = judge_current_contract(
                [event], temporal_classifications=[record], policy_id=policy_id,
                policy_path=policy_path)
        judged = result["events"][0]
        self.assertEqual(result["result"], "PASS")
        self.assertEqual(judged["verification_closure_proof"][
            "classified_video_frame_indices"], record["video_frame_indices"])
        self.assertEqual(judged["closing_current_correct"]["raw_status"], "CURRENT")
        self.assertEqual(judged["closing_current_correct"]["capture_ns"], ANCHOR + 305 * MS)
        self.assertEqual([point["raw_status"] for point in run],
                         ["UNRESOLVED", "UNRESOLVED"])

    def test_27f_closure_requires_complete_context_and_raw_current_bracket(self):
        for mutation, expected in (("missing_context", "INCONCLUSIVE"),
                                   ("wrong_right", "INCONCLUSIVE")):
            with self.subTest(mutation=mutation):
                event = make_event(first_current_ns=20 * MS)
                run = [set_observation(event, offset * MS, "UNRESOLVED",
                                       fields=("primary_frequency",))
                       for offset in (295, 300)]
                record = qualification(
                    event, [point["video_frame_index"] for point in run],
                    fields=("primary_frequency",))
                context = [observation_at(event, offset * MS)["video_frame_index"]
                           for offset in (290, 295, 300, 305)]
                if mutation == "missing_context":
                    context.remove(run[-1]["video_frame_index"])
                else:
                    set_observation(event, 305 * MS, "DEFINITE_OTHER",
                                    fields=("primary_frequency",))
                record.update(
                    verification_closure_semantics=(
                        "RAW_CURRENT_BRACKETED_UNRESOLVED_VERIFICATION_BOUNDARY"),
                    context_frame_indices=context,
                    first={key: run[0][key] for key in (
                        "frame_id", "video_frame_index", "source_frame_seq", "capture_ns")},
                    last={key: run[-1][key] for key in (
                        "frame_id", "video_frame_index", "source_frame_seq", "capture_ns")},
                )
                policy_id = "v1-normal-x-k-ka-blink96-v3"
                with policy_allowing(
                        TEST_CLASSIFIER,
                        fields_by_classifier={TEST_CLASSIFIER: ("primary_frequency",)},
                        closure_by_classifier={TEST_CLASSIFIER:
                            "RAW_CURRENT_BRACKETED_UNRESOLVED_VERIFICATION_BOUNDARY"},
                        policy_id=policy_id) as policy_path:
                    judged = judge_current_contract(
                        [event], temporal_classifications=[record], policy_id=policy_id,
                        policy_path=policy_path)
                self.assertEqual(judged["result"], expected)

    def test_27g_contract_v1_classifier_without_deadline_semantics_still_evaluates(self):
        event = make_event(first_current_ns=20 * MS)
        event["end_ns"] = ANCHOR + 302 * MS
        event["selection_window"]["end_ns"] = ANCHOR + 302 * MS
        event["observations"] = [
            item for item in event["observations"]
            if item["capture_ns"] < ANCHOR + 302 * MS
        ]
        refresh_coverage(event)
        frame = set_observation(event, 100 * MS, "UNRESOLVED", fields=("main_arrows",))
        record = qualification(
            event, [frame["video_frame_index"]],
            classifier_id="v1-arrow-phase-edge-v2", fields=("main_arrows",))
        record["classifier_spec_sha256"] = (
            "31bd3c3e4e470036813e0016ed807828367b0e1b7f18d39d1b13f0a2b4a46edd")

        result = judge_current_contract(
            [event], temporal_classifications=[record],
            policy_id="v1-normal-x-k-ka-blink96-v1")

        self.assertEqual((result["result"], result["reason_code"]),
                         ("PASS", "ALL_REQUIRED_EVENTS_PASSED"))
        self.assertEqual(result["events"][0]["reason_code"],
                         "PRESENTED_BY_DEADLINE_AND_VERIFIED")
        self.assertEqual(result["execution"]["temporal_classification_errors"], [])

    def test_27f_record_carried_semantics_must_match_policy(self):
        event = make_event(first_current_ns=20 * MS)
        frame = set_observation(event, 105 * MS, "UNRESOLVED", fields=("main_arrows",))
        record = qualification(event, [frame["video_frame_index"]], fields=("main_arrows",))
        record["deadline_observation_semantics"] = "TARGET_ACQUISITION_TRANSITION"
        with policy_allowing(
                TEST_CLASSIFIER,
                fields_by_classifier={TEST_CLASSIFIER: ("main_arrows",)},
                semantics_by_classifier={
                    TEST_CLASSIFIER: "LEGAL_PRESENTATION_TRANSITION"}) as policy_path:
            result = judge_visible_event_presentation(
                [event], temporal_classifications=[record], policy_path=policy_path)
        self.assertEqual(result["result"], "INCONCLUSIVE")
        self.assertEqual(
            result["execution"]["temporal_classification_errors"][0]["code"],
            "TEMPORAL_CLASSIFIER_SPEC_MISMATCH")

    def test_28_record_field_must_exist_on_every_claimed_raw_frame(self):
        event = make_event(first_current_ns=20 * MS)
        frames = [
            set_observation(event, 95 * MS, "UNRESOLVED",
                            fields=("main_arrows", "main_bars")),
            set_observation(event, 100 * MS, "UNRESOLVED", fields=("main_arrows",)),
        ]
        record = qualification(event, [item["video_frame_index"] for item in frames],
                               fields=("main_arrows", "main_bars"))
        with policy_allowing(
                TEST_CLASSIFIER,
                fields_by_classifier={TEST_CLASSIFIER: ("main_arrows", "main_bars")}) as policy_path:
            result = judge_visible_event_presentation(
                [event], temporal_classifications=[record], policy_path=policy_path)
        errors = result["execution"]["temporal_classification_errors"]
        self.assertEqual((result["result"], result["events"][0]["reason_code"]),
                         ("INCONCLUSIVE", "UNCLASSIFIED_VISIBLE_INTERVAL"))
        self.assertEqual([item["code"] for item in errors],
                         ["TEMPORAL_CLASSIFICATION_RAW_MISMATCH"])
        self.assertEqual(result["events"][0]["temporal_classifications"], [])


class AuxiliaryVerificationClosureTests(unittest.TestCase):
    POLICY = "v1-normal-x-k-ka-blink96-v3"
    SEMANTICS = "RAW_CURRENT_BRACKETED_UNRESOLVED_VERIFICATION_BOUNDARY"

    def fixture(self, *, tail_refusal=False):
        event = make_event(first_current_ns=20 * MS)
        event["end_ns"] = ANCHOR + 500 * MS
        last = event["observations"][-1]
        tail = []
        for step, offset in enumerate(range(315, 392, 5), 1):
            point = copy.deepcopy(last)
            point.update(frame_id=f"tail-{step}", video_frame_index=last["video_frame_index"] + step,
                         source_frame_seq=last["source_frame_seq"] + step,
                         capture_ns=ANCHOR + offset * MS)
            tail.append(point)
        event["closure_context"] = {
            "selection_window": {"start_ns": ANCHOR + 312 * MS, "end_ns": ANCHOR + 392 * MS},
            "observations": tail,
        }
        run = [set_observation(event, offset * MS, "UNRESOLVED", fields=("secondary",))
               for offset in (295, 300, 305, 310)]
        if tail_refusal:
            tail[0].update(raw_status="UNRESOLVED", raw_affected_fields=["secondary"])
            tail[0].pop("joint_state_id", None)
            run.append(tail[0])
        record = qualification(event, [point["video_frame_index"] for point in run],
                               fields=("secondary",))
        record.update(
            verification_closure_semantics=self.SEMANTICS,
            auxiliary_closure_context_ns=80 * MS,
            context_frame_indices=list(range(run[0]["video_frame_index"] - 1,
                                             run[-1]["video_frame_index"] + 2)),
            first=copy.deepcopy(run[0]), last=copy.deepcopy(run[-1]),
        )
        return event, record

    def judge(self, event, record, *, generic=False, unqualified=False):
        with policy_allowing(
                *([] if unqualified else [TEST_CLASSIFIER]),
                fields_by_classifier={TEST_CLASSIFIER: ("secondary",)},
                closure_by_classifier={} if generic else {TEST_CLASSIFIER: self.SEMANTICS},
                auxiliary_by_classifier={} if generic else {TEST_CLASSIFIER: 80 * MS},
                policy_id=self.POLICY) as policy_path:
            return judge_current_contract(
                [event], temporal_classifications=[record], policy_id=self.POLICY,
                policy_path=policy_path)

    def test_complete_secondary_run_can_close_only_with_separate_auxiliary_current(self):
        for tail_refusal in (False, True):
            with self.subTest(tail_refusal=tail_refusal):
                event, record = self.fixture(tail_refusal=tail_refusal)
                frozen = copy.deepcopy(event)
                result = self.judge(event, record)
                judged = result["events"][0]
                self.assertEqual(result["result"], "PASS", judged["reasons"])
                self.assertEqual(judged["verification_end_ns"], ANCHOR + 292 * MS)
                self.assertEqual(judged["window"]["selection_end_ns"], ANCHOR + 312 * MS)
                self.assertEqual(judged["closing_current_correct"]["capture_ns"],
                                 ANCHOR + (320 if tail_refusal else 315) * MS)
                self.assertEqual(judged["first_current_correct"]["capture_ns"], ANCHOR + 100 * MS)
                self.assertEqual(judged["raw_observations"], frozen["observations"])
                self.assertEqual(event, frozen)
                self.assertTrue(all(point["capture_ns"] < ANCHOR + 312 * MS
                                    for point in judged["derived_observations"]))

    def test_auxiliary_closure_refuses_incomplete_or_contradictory_evidence(self):
        for mutation in ("missing_tail", "skipped_video", "skipped_source", "source_gap",
                         "uncovered_field", "previous", "other", "wrong_context",
                         "nonmaximal_run", "next_input", "extra_tail", "generic", "unqualified"):
            with self.subTest(mutation=mutation):
                event, record = self.fixture(tail_refusal=True)
                tail = event["closure_context"]["observations"]
                if mutation == "missing_tail":
                    event.pop("closure_context")
                elif mutation == "skipped_video":
                    tail.pop(0)
                elif mutation == "skipped_source":
                    tail[1]["source_frame_seq"] += 1
                elif mutation == "source_gap":
                    tail[1]["capture_ns"] += 11 * MS
                elif mutation == "uncovered_field":
                    tail[0]["raw_affected_fields"].append("primary_frequency")
                elif mutation in {"previous", "other"}:
                    tail[1]["raw_status"] = "PREVIOUS" if mutation == "previous" else "DEFINITE_OTHER"
                    tail[1]["raw_affected_fields"] = ["secondary"]
                elif mutation == "wrong_context":
                    record["context_frame_indices"].pop()
                elif mutation == "nonmaximal_run":
                    record["video_frame_indices"].pop()
                elif mutation == "next_input":
                    event["end_ns"] = ANCHOR + 320 * MS
                elif mutation == "extra_tail":
                    tail[-1]["capture_ns"] = ANCHOR + 392 * MS
                elif mutation == "generic":
                    record.pop("verification_closure_semantics")
                    record.pop("auxiliary_closure_context_ns")
                result = self.judge(event, record, generic=mutation == "generic",
                                    unqualified=mutation == "unqualified")
                self.assertNotEqual(result["result"], "PASS")

    def test_auxiliary_current_cannot_establish_late_acquisition_or_hide_core_failure(self):
        for status in ("DEFINITE_OTHER", "PREVIOUS", "UNRESOLVED"):
            with self.subTest(status=status):
                event, record = self.fixture()
                set_observation(event, 100 * MS, status, fields=("secondary",))
                result = self.judge(event, record)
                self.assertNotEqual(result["result"], "PASS")
                if status != "UNRESOLVED":
                    self.assertEqual(result["result"], "FAIL")


if __name__ == "__main__":
    unittest.main()
