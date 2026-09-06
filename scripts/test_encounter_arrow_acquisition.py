#!/usr/bin/env python3
"""Controls for one-way arrow target-acquisition evidence."""
from copy import deepcopy
import hashlib
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent / "bench"))
import encounter_arrow_acquisition as arrow


OFF = [10.0] * 16
ON = [10.0, 90.0, 90.0, 10.0, 10.0, 90.0, 90.0, 10.0,
      90.0, 90.0, 90.0, 90.0, 90.0, 90.0, 90.0, 90.0]


def lerp(left, right, alpha):
    return [a + alpha * (b - a) for a, b in zip(left, right)]


def expected(previous=(("front",),), current=(("side",),)):
    return {
        "input": {"ready": True},
        "fields": {"main_arrows": {"allowed": [list(value) for value in current]}},
        "previous_input": {
            "fields": {"main_arrows": {"allowed": [list(value) for value in previous]}}},
    }


def direction(state, profile, offset):
    return {"state": state, "profile": {"rows": 4, "columns": 4,
            "reference_bounds": [offset, 0, offset + 4, 4],
            "max_channel_medians": list(profile)}}


def sample(index, state, value, profiles, *, expectation=None, difference=False):
    directions = {}
    for offset, name in enumerate(("front", "side", "rear")):
        present = value is not None and name in value
        direction_state = ("filled" if present else "unlit") if state == "readable" else "partial"
        directions[name] = direction(direction_state, profiles[name], offset * 10)
    checks = {"main_arrows": {"status": "UNRESOLVED" if state == "ambiguous" else "MATCH"},
              "main_bars": {"status": "DIFFERENCE" if difference else "MATCH"}}
    return {
        "frame_id": str(index), "video_frame_index": index,
        "source_frame_seq": index + 1, "capture_ns": index * 5_000_000,
        "expected": expected() if expectation is None else expectation,
        "comparison": {"status": "INCONCLUSIVE" if state == "ambiguous" else "PASS",
                       "checks": checks},
        "observed": {"fields": {"main_arrows": {
            "state": state, "value": value, "direction_states": directions,
            "reason": "arrow transition" if state == "ambiguous" else None}}},
    }


def union_chain():
    union = {"front": ON, "side": ON, "rear": OFF}
    end = {"front": OFF, "side": ON, "rear": OFF}
    values = [
        sample(0, "readable", ["front", "side"], union),
        sample(1, "readable", ["front", "side"], union),
        sample(2, "ambiguous", None,
               {"front": lerp(ON, OFF, .25), "side": ON, "rear": OFF}),
        sample(3, "ambiguous", None,
               {"front": lerp(ON, OFF, .75), "side": ON, "rear": OFF}),
        sample(4, "readable", ["side"], end),
        sample(5, "readable", ["side"], end),
    ]
    for item, alpha in zip(values[2:4], (.25, .75)):
        item["observed"]["fields"]["main_arrows"]["direction_states"]["side"]["state"] = "filled"
        item["observed"]["fields"]["main_arrows"]["direction_states"]["rear"]["state"] = "unlit"
    return values


def direct_chain():
    result = [
        sample(0, "readable", ["front"], {"front": ON, "side": OFF, "rear": OFF}),
        sample(1, "readable", ["front"], {"front": ON, "side": OFF, "rear": OFF}),
    ]
    for index, alpha in enumerate((.2, .6, .9), 2):
        result.append(sample(index, "ambiguous", None, {
            "front": lerp(ON, OFF, alpha), "side": lerp(OFF, ON, alpha), "rear": OFF}))
        result[-1]["observed"]["fields"]["main_arrows"]["direction_states"]["rear"]["state"] = "unlit"
    result.extend([
        sample(5, "readable", ["side"], {"front": OFF, "side": ON, "rear": OFF}),
        sample(6, "readable", ["side"], {"front": OFF, "side": ON, "rear": OFF}),
    ])
    return result


def context(**changes):
    value = {"capture_id": "a" * 64, "selection_manifest_sha256": "b" * 64,
             "verified_maximum_source_interval_ns": 10_000_000,
             "reader_method_version": arrow.PROFILE_READER_METHOD_VERSION,
             "reader_sha256": arrow.PROFILE_READER_SHA256}
    value.update(changes)
    return value


def classify(samples):
    return arrow.classify_arrow_acquisition_runs(
        samples, [{"event_id": "event-0001", "start_ns": 0, "end_ns": 1_000_000_000}],
        context())


class ArrowAcquisitionTests(unittest.TestCase):
    def test_rejections_keep_the_same_pre_gate_scope_as_admissions(self):
        samples = union_chain()
        admitted = classify(samples)["classifications"][0]
        for failure in ("expectation", "claim", "motion"):
            with self.subTest(failure=failure):
                rejected_samples = deepcopy(samples)
                if failure == "expectation":
                    rejected_samples[2]["expected"] = {}
                elif failure == "claim":
                    for item in rejected_samples[2:4]:
                        item["comparison"]["checks"]["main_bars"]["status"] = "DIFFERENCE"
                else:
                    rejected_samples[2]["observed"]["fields"]["main_arrows"]["direction_states"][
                        "front"]["profile"]["max_channel_medians"] = [255.0] * 16
                result = classify(rejected_samples)
                self.assertEqual(result["classifications"], [])
                rejected = result["rejected_runs"][0]
                for name in ("full_transition_indices", "left_support", "right_support"):
                    self.assertEqual(rejected[name], admitted[name])

    def test_missing_support_is_explicit_on_each_side(self):
        for start, stop, missing in ((1, 6, {"left"}), (0, 5, {"right"}),
                                     (1, 5, {"left", "right"})):
            with self.subTest(missing=missing):
                result = classify(union_chain()[start:stop])
                self.assertEqual(result["classifications"], [])
                rejected = result["rejected_runs"][0]
                self.assertEqual(rejected["code"], "UNCLOSED_RUN")
                self.assertEqual(rejected["full_transition_indices"], [2, 3])
                for side, expected in (("left", [0, 1]), ("right", [4, 5])):
                    self.assertEqual([point["video_frame_index"]
                                      for point in rejected[f"{side}_support"]],
                                     [] if side in missing else expected)

    def test_local_unlit_endpoint_is_not_replaced_by_later_rear_plateau(self):
        samples = union_chain()
        dark = {"front": OFF, "side": OFF, "rear": OFF}
        samples[4:] = [sample(index, "readable", [], dark) for index in (4, 5)]
        samples.extend(sample(index, "readable", ["rear"],
                              {"front": OFF, "side": OFF, "rear": ON})
                       for index in (6, 7))
        for item in samples:
            item["expected"] = expected(previous=(("front", "side"),), current=(("rear",),))
        result = classify(samples)
        self.assertEqual(result["classifications"], [])
        rejected = result["rejected_runs"][0]
        self.assertEqual(rejected["code"], "NOT_ACQUISITION_ENDPOINTS")
        self.assertEqual(rejected["full_transition_indices"], [2, 3])
        self.assertEqual([point["video_frame_index"] for point in rejected["right_support"]], [4, 5])

    def test_outgoing_union_fade_is_one_way_failure_evidence(self):
        samples = union_chain()
        frozen = deepcopy(samples)
        result = classify(samples)
        self.assertEqual(result["errors"], [])
        self.assertEqual(result["rejected_runs"], [])
        self.assertEqual(len(result["classifications"]), 1)
        record = result["classifications"][0]
        self.assertEqual(record["video_frame_indices"], [2, 3])
        self.assertEqual(record["raw_affected_fields"], ["main_arrows"])
        self.assertEqual(record["deadline_observation_semantics"],
                         "TARGET_ACQUISITION_TRANSITION")
        self.assertEqual(record["changed_directions"], ["front"])
        self.assertEqual(
            record["claimed_frame_acquisition_proof"],
            [
                {"video_frame_index": 2,
                 "changed_direction_states": {"front": "partial"},
                 "noncurrent_changed_directions": ["front"]},
                {"video_frame_index": 3,
                 "changed_direction_states": {"front": "partial"},
                 "noncurrent_changed_directions": ["front"]},
            ])
        self.assertEqual(samples, frozen)

    def test_direct_crossfade_allows_asynchronous_changed_directions(self):
        result = classify(direct_chain())
        self.assertEqual(result["rejected_runs"], [])
        self.assertEqual(result["classifications"][0]["changed_directions"], ["front", "side"])

    def test_spatially_progressive_glyph_draw_is_bounded_but_not_uniform(self):
        samples = direct_chain()
        profiles = samples[3]["observed"]["fields"]["main_arrows"]["direction_states"]
        profiles["front"]["profile"]["max_channel_medians"] = [
            value if index % 2 else OFF[index]
            for index, value in enumerate(ON)
        ]
        profiles["side"]["profile"]["max_channel_medians"] = [
            value if index % 2 else ON[index]
            for index, value in enumerate(OFF)
        ]
        result = classify(samples)
        self.assertEqual(result["rejected_runs"], [])
        metrics = result["classifications"][0]["direction_metrics"]
        self.assertGreater(metrics["front"]["normalized_residuals"][1], 0.15)

        profiles["front"]["profile"]["max_channel_medians"] = [
            255.0 if index % 2 else 0.0 for index in range(16)
        ]
        rejected = classify(samples)
        self.assertEqual(rejected["classifications"], [])
        self.assertIn(rejected["rejected_runs"][0]["code"],
                      {"PROJECTION_RANGE", "NORMALIZED_RESIDUAL"})

    def test_nearest_current_phase_pair_is_not_replaced_by_farther_prior_content(self):
        samples = direct_chain()
        current = {"front": OFF, "side": ON, "rear": OFF}
        samples[2:2] = [
            sample(2, "readable", ["side"], current),
            sample(3, "readable", ["side"], current),
        ]
        for index, item in enumerate(samples):
            item["frame_id"] = str(index)
            item["video_frame_index"] = index
            item["source_frame_seq"] = index + 1
            item["capture_ns"] = index * 5_000_000
        result = classify(samples)
        self.assertEqual(result["classifications"], [])
        self.assertEqual(result["rejected_runs"][0]["code"], "NOT_ACQUISITION_ENDPOINTS")

    def test_noncurrent_evidence_elsewhere_cannot_emit_a_product_claim(self):
        for status in ("DIFFERENCE", "TRANSITION_DIFFERENCE", "PREVIOUS_INPUT_STATE"):
            with self.subTest(status=status):
                samples = union_chain()
                for item in samples[2:4]:
                    item["comparison"]["checks"]["main_bars"]["status"] = status
                result = classify(samples)
                self.assertEqual(result["classifications"], [])
                self.assertEqual(result["rejected_runs"][0]["code"], "NO_PRODUCT_CLAIM")

    def test_every_claimed_frame_must_still_have_a_noncurrent_changed_direction(self):
        samples = direct_chain()
        current = {"front": OFF, "side": ON, "rear": OFF}
        for item in samples[2:5]:
            directions = item["observed"]["fields"]["main_arrows"]["direction_states"]
            for name in ("front", "side"):
                directions[name]["state"] = "filled" if name == "side" else "unlit"
                directions[name]["profile"]["max_channel_medians"] = list(current[name])
        result = classify(samples)
        self.assertEqual(result["classifications"], [])
        self.assertEqual(result["rejected_runs"][0]["code"],
                         "CLAIMED_FRAME_AT_CURRENT_ENDPOINT")

    def test_backtrack_gap_and_changed_expectation_are_refused(self):
        backtrack = direct_chain()
        for direction_name, left, right in (("front", ON, OFF), ("side", OFF, ON)):
            backtrack[3]["observed"]["fields"]["main_arrows"]["direction_states"] \
                [direction_name]["profile"]["max_channel_medians"] = lerp(left, right, .9)
            backtrack[4]["observed"]["fields"]["main_arrows"]["direction_states"] \
                [direction_name]["profile"]["max_channel_medians"] = lerp(left, right, .6)
        gap = direct_chain()
        gap[3]["source_frame_seq"] += 1
        changed = direct_chain()
        changed[3]["expected"] = expected(previous=(("rear",),), current=(("side",),))
        self.assertEqual(classify(backtrack)["rejected_runs"][0]["code"],
                         "MAXIMUM_BACKWARD_STEP")
        self.assertEqual(classify(gap)["rejected_runs"][0]["code"], "SOURCE_GAP")
        self.assertEqual(classify(changed)["rejected_runs"][0]["code"],
                         "EXPECTATION_SIGNATURE")

    def test_spec_and_context_are_exactly_bound(self):
        spec = Path(arrow.__file__).with_name("temporal_specs") / (
            arrow.CLASSIFIER_ID + ".json")
        self.assertEqual(arrow.CLASSIFIER_SPEC_SHA256, hashlib.sha256(spec.read_bytes()).hexdigest())
        bad = arrow.classify_arrow_acquisition_runs(
            union_chain(), [{"event_id": "e", "start_ns": 0, "end_ns": 100_000_000}],
            context(reader_sha256="0" * 64))
        self.assertEqual(bad["classifications"], [])
        self.assertTrue(bad["errors"])


if __name__ == "__main__":
    unittest.main()
