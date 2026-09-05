#!/usr/bin/env python3
"""Frozen arrow phase-edge classifier controls and refusal boundaries."""
from copy import deepcopy
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent / "bench"))
import encounter_arrow_transition as arrow


OFF = [10.0] * 16
ON = [10.0, 90.0, 90.0, 10.0, 10.0, 90.0, 90.0, 10.0,
      90.0, 90.0, 90.0, 90.0, 90.0, 90.0, 90.0, 90.0]


def lerp(left, right, alpha):
    return [a + alpha * (b - a) for a, b in zip(left, right)]


def endpoint_at_separation(separation):
    amplitude = separation / ((12 / 16) ** 0.5)
    return [10.0 if value == 10 else 10.0 + amplitude for value in ON]


def expected(sets=([], ["front"])):
    return {"input": {"ready": True}, "fields": {"main_arrows": {"allowed": list(sets)}},
            "joint_states": [{"main_arrows": value} for value in sets]}


def direction(state, profile, offset=0):
    return {"state": state, "profile": {"rows": 4, "columns": 4,
            "reference_bounds": [offset, 0, offset + 4, 4],
            "max_channel_medians": list(profile)}}


def sample(index, state, value, front, *, capture_ns=None, expected_value=None,
           side_profile=OFF, source_seq=None):
    directions = {
        "front": direction("filled" if value and "front" in value else
                           "unlit" if state == "readable" else "partial", front),
        "side": direction("unlit", side_profile, 10),
        "rear": direction("unlit", OFF, 20),
    }
    reading = {"state": state, "value": value,
               "direction_states": directions,
               "reason": "front arrow partial" if state == "ambiguous" else None}
    return {"frame_id": str(index), "video_frame_index": index,
            "source_frame_seq": index + 1 if source_seq is None else source_seq,
            "capture_ns": index * 5_000_000 if capture_ns is None else capture_ns,
            "expected": expected() if expected_value is None else expected_value,
            "observed": {"fields": {"main_arrows": reading}}}


def chain(alphas=(0.2, 0.6, 0.95)):
    result = [sample(0, "readable", [], OFF), sample(1, "readable", [], OFF)]
    for index, alpha in enumerate(alphas, 2):
        result.append(sample(index, "ambiguous", None, lerp(OFF, ON, alpha)))
    end = len(result)
    result.extend([sample(end, "readable", ["front"], ON),
                   sample(end + 1, "readable", ["front"], ON)])
    return result


def context(**changes):
    value = {"capture_id": "a" * 64, "selection_manifest_sha256": "b" * 64,
             "verified_maximum_source_interval_ns": 10_000_000,
             "reader_method_version": arrow.PROFILE_READER_METHOD_VERSION,
             "reader_sha256": arrow.PROFILE_READER_SHA256}
    value.update(changes)
    return value


def classify(samples):
    return arrow.classify_arrow_runs(samples, [{"event_id": "event-0001",
                                                "start_ns": 0, "end_ns": 1_000_000_000}],
                                     context())


class ArrowTransitionTests(unittest.TestCase):
    def test_ideal_monotone_edge_is_qualified_without_mutating_raw_frames(self):
        samples = chain()
        frozen = deepcopy(samples)
        result = classify(samples)
        self.assertEqual(result["errors"], [])
        self.assertEqual(result["rejected_runs"], [])
        self.assertEqual(len(result["classifications"]), 1)
        record = result["classifications"][0]
        self.assertEqual(record["classifier_id"], arrow.CLASSIFIER_ID)
        self.assertEqual(record["classifier_spec_sha256"], arrow.CLASSIFIER_SPEC_SHA256)
        self.assertEqual(record["video_frame_indices"], [2, 3, 4])
        self.assertEqual(record["raw_affected_fields"], ["main_arrows"])
        self.assertEqual(samples, frozen)

    def test_v2_identity_and_endpoint_separation_boundary_are_frozen(self):
        self.assertEqual(arrow.CLASSIFIER_ID, "v1-arrow-phase-edge-v2")
        self.assertEqual(arrow.ENDPOINT_SEPARATION_RMS_MIN, 52.0)
        boundary = endpoint_at_separation(52.0)
        below = endpoint_at_separation(51.999)
        boundary_samples = chain((0.5,))
        below_samples = chain((0.5,))
        for item in boundary_samples[-2:]:
            item["observed"]["fields"]["main_arrows"]["direction_states"]["front"] \
                ["profile"]["max_channel_medians"] = boundary
        for item in below_samples[-2:]:
            item["observed"]["fields"]["main_arrows"]["direction_states"]["front"] \
                ["profile"]["max_channel_medians"] = below
        qualified = classify(boundary_samples)
        refused = classify(below_samples)
        self.assertEqual(len(qualified["classifications"]), 1)
        self.assertAlmostEqual(
            qualified["classifications"][0]["endpoint_separation_rms"], 52.0)
        self.assertEqual(refused["rejected_runs"][0]["code"], "ENDPOINT_SEPARATION")

    def test_low_endpoint_separation_and_fragment_shape_are_refused(self):
        weak = [10.0 if value == 10 else 31.0 for value in ON]
        weak_samples = chain((0.5,))
        for item in weak_samples[-2:]:
            item["observed"]["fields"]["main_arrows"]["direction_states"]["front"] \
                ["profile"]["max_channel_medians"] = weak
        fragment = chain((0.5,))
        broken = list(OFF)
        for index in (1, 2, 5, 6):
            broken[index] = ON[index]
        fragment[2]["observed"]["fields"]["main_arrows"]["direction_states"]["front"] \
            ["profile"]["max_channel_medians"] = broken
        self.assertEqual(classify(weak_samples)["rejected_runs"][0]["code"], "ENDPOINT_SEPARATION")
        self.assertEqual(classify(fragment)["rejected_runs"][0]["code"], "NORMALIZED_RESIDUAL")

    def test_projection_backtracking_and_extra_direction_motion_are_refused(self):
        projected = chain((1.06,))
        backtrack = chain((0.2, 0.8, 0.7))
        extra = chain((0.5,))
        extra[2]["observed"]["fields"]["main_arrows"]["direction_states"]["side"] \
            ["profile"]["max_channel_medians"] = [18.01] * 16
        self.assertEqual(classify(projected)["rejected_runs"][0]["code"], "PROJECTION_RANGE")
        self.assertEqual(classify(backtrack)["rejected_runs"][0]["code"], "MAXIMUM_BACKWARD_STEP")
        self.assertEqual(classify(extra)["rejected_runs"][0]["code"], "EXTRA_DIRECTION_MOTION")

    def test_open_source_gap_long_span_and_expectation_change_are_refused(self):
        open_run = chain()[1:]
        gap = chain()
        gap[3]["source_frame_seq"] += 1
        long_span = chain()
        for index, item in enumerate(long_span):
            item["capture_ns"] = index * 30_000_000
        changed = chain()
        changed[3]["expected"] = expected(([], ["side"]))
        self.assertEqual(classify(open_run)["rejected_runs"][0]["code"], "UNCLOSED_RUN")
        self.assertEqual(classify(gap)["rejected_runs"][0]["code"], "SOURCE_GAP")
        self.assertEqual(classify(long_span)["rejected_runs"][0]["code"], "SOURCE_GAP")
        self.assertEqual(classify(changed)["rejected_runs"][0]["code"], "EXPECTATION_SIGNATURE")

    def test_only_changed_direction_may_be_ambiguous(self):
        samples = chain((0.5,))
        samples[2]["observed"]["fields"]["main_arrows"]["direction_states"]["side"]["state"] = "faint"
        self.assertEqual(classify(samples)["rejected_runs"][0]["code"], "EXTRA_DIRECTION_STATE")

    def test_context_must_bind_exact_frozen_reader(self):
        result = arrow.classify_arrow_runs(chain(), [{"event_id": "e", "start_ns": 0,
                                                      "end_ns": 100_000_000}],
                                           context(reader_sha256="0" * 64))
        self.assertEqual(result["classifications"], [])
        self.assertTrue(result["errors"])


if __name__ == "__main__":
    unittest.main()
