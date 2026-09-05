#!/usr/bin/env python3
"""Frozen arrow phase-edge classifier controls and refusal boundaries."""
from copy import deepcopy
import json
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


def fading_chain():
    return [
        sample(0, "readable", ["front"], ON),
        sample(1, "readable", ["front"], lerp(ON, OFF, 0.55)),
        sample(2, "ambiguous", None, lerp(ON, OFF, 0.80)),
        sample(3, "ambiguous", None, lerp(ON, OFF, 0.95)),
        sample(4, "readable", [], OFF),
        sample(5, "readable", [], OFF),
    ]


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
    def test_qualification_requires_metrics_for_both_readable_inner_frames(self):
        import encounter_qualification as qualification
        samples = fading_chain()
        record = classify(samples)["classifications"][0]
        source_rows = {item["video_frame_index"]: {
            "source_frame_seq": item["source_frame_seq"], "capture_ns": item["capture_ns"]}
            for item in samples}
        spec = json.loads((Path(arrow.__file__).with_name("temporal_specs") /
                           "v1-arrow-phase-edge-v4.json").read_text())

        def validate(candidate):
            qualification._validate_temporal_v2_arrow_record(
                candidate, record["video_frame_indices"], context(), spec, source_rows)

        validate(record)
        for field in ("profile_frame_indices", "projections", "normalized_residuals"):
            with self.subTest(field=field):
                candidate = deepcopy(record)
                candidate[field] = candidate[field][1:-1]
                with self.assertRaises(qualification.QualificationError):
                    validate(candidate)
        candidate = deepcopy(record)
        candidate["normalized_residuals"][0] = 0.151
        with self.assertRaises(qualification.QualificationError):
            validate(candidate)

    def test_whole_fixed_chain_measures_already_fading_readable_endpoint(self):
        samples = fading_chain()
        frozen = deepcopy(samples)
        nearest = samples[1]["observed"]["fields"]["main_arrows"]["direction_states"] \
            ["front"]["profile"]["max_channel_medians"]
        self.assertLess(arrow._rms([a - b for a, b in zip(nearest, OFF)]),
                        arrow.ENDPOINT_SEPARATION_RMS_MIN)
        result = classify(samples)
        self.assertEqual(result["errors"], [])
        self.assertEqual(result["rejected_runs"], [])
        record = result["classifications"][0]
        self.assertEqual(record["video_frame_indices"], [2, 3])
        self.assertEqual(record["profile_frame_indices"], [1, 2, 3, 4])
        self.assertEqual(record["left_support"]["video_frame_index"], 0)
        self.assertEqual(record["right_support"]["video_frame_index"], 5)
        for actual, expected_alpha in zip(record["projections"], [0.55, 0.8, 0.95, 1.0]):
            self.assertAlmostEqual(actual, expected_alpha)
        self.assertEqual(samples, frozen)

    def test_readable_inner_frame_cannot_hide_a_shape_excursion_or_backtrack(self):
        for code in ("NORMALIZED_RESIDUAL", "MAXIMUM_BACKWARD_STEP"):
            with self.subTest(code=code):
                samples = fading_chain()
                profile = samples[1]["observed"]["fields"]["main_arrows"] \
                    ["direction_states"]["front"]["profile"]
                if code == "MAXIMUM_BACKWARD_STEP":
                    profile["max_channel_medians"] = lerp(ON, OFF, 0.90)
                else:
                    for index in (0, 3, 4, 7):
                        profile["max_channel_medians"][index] += 30.0
                result = classify(samples)
                self.assertEqual(result["classifications"], [])
                self.assertEqual(result["rejected_runs"][0]["code"], code)

    def test_extra_direction_motion_in_readable_inner_frame_is_refused(self):
        samples = fading_chain()
        samples[1]["observed"]["fields"]["main_arrows"]["direction_states"]["side"] \
            ["profile"]["max_channel_medians"] = [18.01] * 16
        self.assertEqual(classify(samples)["rejected_runs"][0]["code"],
                         "EXTRA_DIRECTION_MOTION")

    def test_low_separation_does_not_search_earlier_bright_frames(self):
        samples = fading_chain()
        samples[0]["observed"]["fields"]["main_arrows"]["direction_states"]["front"] \
            ["profile"]["max_channel_medians"] = lerp(ON, OFF, 0.50)
        for item in samples:
            item["video_frame_index"] += 1
            item["source_frame_seq"] += 1
            item["capture_ns"] += 5_000_000
        samples.insert(0, sample(0, "readable", ["front"], ON))
        self.assertEqual(classify(samples)["rejected_runs"][0]["code"],
                         "ENDPOINT_SEPARATION")

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

    def test_v4_preserves_numeric_endpoint_separation_boundary(self):
        self.assertEqual(arrow.CLASSIFIER_ID, "v1-arrow-phase-edge-v4")
        import hashlib
        spec = Path(arrow.__file__).with_name("temporal_specs") / "v1-arrow-phase-edge-v4.json"
        self.assertEqual(arrow.CLASSIFIER_SPEC_SHA256, hashlib.sha256(spec.read_bytes()).hexdigest())
        previous_spec = spec.with_name("v1-arrow-phase-edge-v3.json")
        self.assertEqual(hashlib.sha256(previous_spec.read_bytes()).hexdigest(),
                         "88bbfe87c6e1c54f7578e2f2380a5490b59ba977acefef34c811964af1fb9849")
        self.assertEqual(json.loads(spec.read_text())["constants"],
                         json.loads(previous_spec.read_text())["constants"])
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
