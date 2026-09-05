#!/usr/bin/env python3
"""Candidate adjacent main-bar redraw gates and refusal boundaries."""
from copy import deepcopy
import hashlib
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts" / "bench"))
import encounter_bar_transition as bars
import encounter_temporal as temporal


OFF = [10.0] * 16
ON = [90.0] * 16


def lerp(left, right, alpha):
    return [a + alpha * (b - a) for a, b in zip(left, right)]


def fill(state, median=None):
    if state == "on":
        return {"state": state, "p10": 70.0, "median": 90.0 if median is None else median,
                "p90": 90.0}
    if state == "off":
        return {"state": state, "p10": 10.0, "median": 12.0 if median is None else median,
                "p90": 14.0}
    return {"state": state, "p10": 20.0, "median": 45.0 if median is None else median,
            "p90": 100.0}


def expected(previous=2, current=3):
    return {"input": {"ready": True}, "fields": {"main_bars": {"allowed": [current]}},
            "previous_input": {"fields": {"main_bars": {"allowed": [previous]}}}}


def sample(index, state, count, cell_states, profiles, *, capture_ns=None,
           source_seq=None, expectation=None, reason=None):
    fills = [fill(value) for value in cell_states]
    if state == "ambiguous":
        boundary = cell_states.index("partial") if "partial" in cell_states else 0
        fills[boundary] = fill("partial", sum(profiles[boundary]) / len(profiles[boundary]))
    raw = [{key: item[key] for key in ("p10", "median", "p90")} for item in fills]
    reading = {"state": state, "value": count,
               "reason": (bars.RAW_AMBIGUOUS_REASON if state == "ambiguous" else None)
                         if reason is None else reason,
               "bars": raw}
    redraw = {"schema_version": 1, "method_version": bars.PROFILE_REDRAW_PROBE_METHOD_VERSION,
              "main_bars": {"profile_schema": deepcopy(bars.PROFILE_SCHEMA), "bars": [
                  {"box": list(box), "fill": item, "profile": list(profile)}
                  for box, item, profile in zip(bars.MAIN_BAR_BOXES, fills, profiles)]}}
    return {"frame_id": str(index), "video_frame_index": index,
            "source_frame_seq": index + 1 if source_seq is None else source_seq,
            "capture_ns": index * 5_000_000 if capture_ns is None else capture_ns,
            "expected": expected() if expectation is None else expectation,
            "observed": {"fields": {"main_bars": reading},
                         "redraw_profiles": redraw}}


def definite(index, count, **changes):
    states = ["on" if cell < count else "off" for cell in range(6)]
    profiles = [ON if value == "on" else OFF for value in states]
    return sample(index, "readable", count, states, profiles, **changes)


def transition(index, alpha, previous=2, current=3, **changes):
    boundary = min(previous, current)
    states = ["on" if cell < boundary else "off" for cell in range(6)]
    states[boundary] = "partial"
    left = ON if previous > current else OFF
    right = ON if current > previous else OFF
    profiles = [ON if value == "on" else OFF for value in states]
    profiles[boundary] = lerp(left, right, alpha)
    return sample(index, "ambiguous", None, states, profiles,
                  expectation=expected(previous, current), **changes)


def chain(alphas=(0.2, 0.6, 0.95), previous=2, current=3):
    result = [definite(0, previous), definite(1, previous)]
    for index, alpha in enumerate(alphas, 2):
        result.append(transition(index, alpha, previous, current))
    end = len(result)
    result.extend([definite(end, current), definite(end + 1, current)])
    for item in result:
        item["expected"] = expected(previous, current)
    return result


def context(**changes):
    value = {"capture_id": "a" * 64, "selection_manifest_sha256": "b" * 64,
             "verified_maximum_source_interval_ns": 10_000_000,
             "reader_method_version": bars.PROFILE_READER_METHOD_VERSION,
             "reader_sha256": bars.PROFILE_READER_SHA256,
             "redraw_probe_method_version": bars.PROFILE_REDRAW_PROBE_METHOD_VERSION,
             "redraw_probe_sha256": bars.PROFILE_REDRAW_PROBE_SHA256}
    value.update(changes)
    return value


def classify(samples, **context_changes):
    return bars.classify_main_bar_runs(
        samples, [{"event_id": "event-0001", "start_ns": 0, "end_ns": 1_000_000_000}],
        context(**context_changes))


class BarTransitionTests(unittest.TestCase):
    def test_candidate_identity_and_numeric_gates_are_explicit(self):
        self.assertEqual(bars.CLASSIFIER_ID, "v1-main-bar-adjacent-redraw-v2")
        spec = ROOT / "scripts" / "bench" / "temporal_specs" / f"{bars.CLASSIFIER_ID}.json"
        self.assertEqual(bars.CLASSIFIER_SPEC_SHA256,
                         hashlib.sha256(spec.read_bytes()).hexdigest())
        self.assertEqual(bars.PROFILE_REDRAW_PROBE_SHA256, hashlib.sha256(
            (ROOT / "scripts" / "bench" / "encounter_redraw_probe.py").read_bytes()).hexdigest())
        self.assertEqual(bars.MAXIMUM_RECORDING_SOURCE_INTERVAL_NS, 1_000_000_000)
        self.assertEqual(bars.MAXIMUM_SUPPORT_CHAIN_INTERVAL_NS, 10_000_000)
        self.assertEqual(bars.AUTHORED_DISPLAY_UPDATE_NS, 50_000_000)
        self.assertEqual(bars.STABLE_SUPPORT_FRAMES_EACH_SIDE, 2)
        self.assertEqual(bars.ENDPOINT_SEPARATION_RMS_MIN, 20.0)
        self.assertEqual((bars.PROJECTION_MIN, bars.PROJECTION_MAX), (-0.05, 1.05))
        self.assertEqual(bars.NORMALIZED_RESIDUAL_MAX, 0.15)
        self.assertEqual(bars.MAXIMUM_BACKWARD_STEP, 0.05)
        self.assertEqual(bars.MAXIMUM_TOTAL_BACKWARD_MOTION, 0.10)
        self.assertEqual(bars.BOUNDARY_MEDIAN_BACKWARD_TOLERANCE, 2.0)
        self.assertEqual(bars.UNCHANGED_CELL_PROFILE_DIAMETER_RMS_MAX, 8.0)

    def test_ideal_adjacent_redraw_is_candidate_without_mutating_raw_frames(self):
        samples = chain()
        frozen = deepcopy(samples)
        result = classify(samples)
        self.assertEqual(result["errors"], [])
        self.assertEqual(result["rejected_runs"], [])
        self.assertEqual(len(result["classifications"]), 1)
        record = result["classifications"][0]
        self.assertEqual(record["classifier_id"], "v1-main-bar-adjacent-redraw-v2")
        self.assertEqual(record["classifier_spec_sha256"], bars.CLASSIFIER_SPEC_SHA256)
        self.assertEqual(record["status"], "QUALIFIED_CAPTURE_TRANSITION")
        self.assertNotIn("candidate_only", record)
        self.assertEqual(record["raw_affected_fields"], ["main_bars"])
        self.assertEqual(record["video_frame_indices"], [2, 3, 4])
        self.assertEqual(record["endpoint_values"], [2, 3])
        self.assertEqual(record["changed_bar_index"], 2)
        self.assertEqual(samples, frozen)

    def test_recording_wide_gap_does_not_loosen_or_reject_local_support(self):
        result = classify(chain(), verified_maximum_source_interval_ns=15_000_000)
        self.assertEqual(result["errors"], [])
        self.assertEqual(len(result["classifications"]), 1, result)
        record = result["classifications"][0]
        self.assertEqual(record["verified_maximum_source_interval_ns"], 15_000_000)
        self.assertEqual(record["maximum_endpoint_span_ns"], 60_000_000)

        local_gap = chain()
        for sample in local_gap[3:]:
            sample["capture_ns"] += 10_000_000
        rejected = classify(
            local_gap, verified_maximum_source_interval_ns=15_000_000)
        self.assertEqual(rejected["errors"], [])
        self.assertEqual(rejected["classifications"], [])
        self.assertEqual(rejected["rejected_runs"][0]["code"], "SOURCE_GAP")

    def test_decrement_and_temporal_integration_emit_the_same_bounded_candidate(self):
        samples = chain((0.25, 0.8), previous=4, current=3)
        direct = classify(samples)
        integrated = temporal.classify_temporal(
            samples, {"events": [{"event_id": "event-0001", "start_ns": 0,
                                   "end_ns": 1_000_000_000}]}, context())
        self.assertEqual(len(direct["classifications"]), 1, direct)
        matches = [item for item in integrated["classifications"]
                   if item.get("classifier_id") == bars.CLASSIFIER_ID]
        self.assertEqual(len(matches), 1, integrated)
        self.assertEqual(matches[0]["endpoint_values"], [4, 3])
        self.assertEqual(matches[0]["raw_affected_fields"], ["main_bars"])

    def test_missing_or_conflicting_probe_evidence_is_refused(self):
        missing = chain((0.5,))
        missing[2]["observed"].pop("redraw_profiles")
        mismatch = chain((0.5,))
        mismatch[2]["observed"]["redraw_profiles"]["main_bars"]["bars"][0]["fill"]["median"] += 1
        wrong_box = chain((0.5,))
        wrong_box[2]["observed"]["redraw_profiles"]["main_bars"]["bars"][0]["box"][0] += 1
        for samples in (missing, mismatch, wrong_box):
            with self.subTest(samples=samples):
                result = classify(samples)
                self.assertEqual(result["classifications"], [])
                self.assertEqual(result["rejected_runs"][0]["code"], "NOT_BOUNDARY_ONLY")

    def test_support_gap_span_endpoint_and_expectation_gates_refuse(self):
        open_run = chain()[1:]
        gap = chain()
        gap[3]["source_frame_seq"] += 1
        long_span = chain((0.5,))
        for index, item in enumerate(long_span):
            item["capture_ns"] = index * 15_000_000
        bounded_gaps_long_span = chain(tuple(index / 13 for index in range(1, 13)))
        nonadjacent = chain((0.5,))
        nonadjacent[-2] = definite(nonadjacent[-2]["video_frame_index"], 4)
        nonadjacent[-1] = definite(nonadjacent[-1]["video_frame_index"], 4)
        for item in nonadjacent:
            item["expected"] = expected(2, 3)
        changed_expectation = chain((0.5,))
        changed_expectation[2]["expected"] = expected(1, 2)
        unstable = chain((0.5,))
        unstable[0] = definite(0, 1)
        unstable[0]["expected"] = expected(2, 3)
        cases = ((open_run, "UNCLOSED_RUN"), (gap, "SOURCE_GAP"),
                 (long_span, "SOURCE_GAP"), (nonadjacent, "NOT_ADJACENT_COUNTS"),
                 (bounded_gaps_long_span, "ENDPOINT_SPAN"),
                 (changed_expectation, "EXPECTATION_SIGNATURE"),
                 (unstable, "UNSTABLE_ENDPOINT"))
        for samples, code in cases:
            with self.subTest(code=code):
                result = classify(samples)
                self.assertEqual(result["classifications"], [])
                self.assertEqual(result["rejected_runs"][0]["code"], code, result)

    def test_only_boundary_cell_may_be_partial_and_other_cells_must_stay_stable(self):
        extra_partial = chain((0.5,))
        item = extra_partial[2]
        raw = item["observed"]["fields"]["main_bars"]["bars"][4]
        raw.update(p10=20.0, median=45.0, p90=70.0)
        fill_item = item["observed"]["redraw_profiles"]["main_bars"]["bars"][4]["fill"]
        fill_item.update(state="partial", p10=20.0, median=45.0, p90=70.0)
        changed_other = chain((0.5,))
        changed_other[2]["observed"]["redraw_profiles"]["main_bars"]["bars"][4]["profile"] = [19.0] * 16
        for samples, code in ((extra_partial, "NOT_BOUNDARY_ONLY"),
                              (changed_other, "UNCHANGED_CELL_MOTION")):
            result = classify(samples)
            self.assertEqual(result["classifications"], [])
            self.assertEqual(result["rejected_runs"][0]["code"], code)

    def test_profile_separation_shape_range_and_backtracking_gates_refuse(self):
        weak = chain((0.5,))
        for item in weak[-2:]:
            item["observed"]["redraw_profiles"]["main_bars"]["bars"][2]["profile"] = [29.999] * 16
        fragment = chain((0.5,))
        fragment[2]["observed"]["redraw_profiles"]["main_bars"]["bars"][2]["profile"] = \
            [50.0] * 8 + [10.0] * 8
        projected = chain((1.06,))
        backtrack = chain((0.2, 0.8, 0.7))
        cumulative_backtrack = chain((0.20, 0.16, 0.40, 0.36, 0.60, 0.56, 0.80))
        cases = ((weak, "ENDPOINT_SEPARATION"), (fragment, "NORMALIZED_RESIDUAL"),
                 (projected, "PROJECTION_RANGE"), (backtrack, "MAXIMUM_BACKWARD_STEP"),
                 (cumulative_backtrack, "TOTAL_BACKWARD_MOTION"))
        for samples, code in cases:
            with self.subTest(code=code):
                result = classify(samples)
                self.assertEqual(result["classifications"], [])
                self.assertEqual(result["rejected_runs"][0]["code"], code, result)

    def test_raw_reason_median_direction_and_exact_context_are_required(self):
        wrong_reason = chain((0.5,))
        wrong_reason[2]["observed"]["fields"]["main_bars"]["reason"] = "another refusal"
        median_backtrack = chain((0.2, 0.6))
        item = median_backtrack[3]
        raw = item["observed"]["fields"]["main_bars"]["bars"][2]
        raw["median"] = 20.0
        item["observed"]["redraw_profiles"]["main_bars"]["bars"][2]["fill"]["median"] = 20.0
        self.assertEqual(classify(wrong_reason)["rejected_runs"][0]["code"], "NOT_BOUNDARY_ONLY")
        self.assertEqual(classify(median_backtrack)["rejected_runs"][0]["code"],
                         "BOUNDARY_MEDIAN_BACKTRACK")
        invalid = bars.classify_main_bar_runs(
            chain(), [{"event_id": "e", "start_ns": 0, "end_ns": 100_000_000}],
            context(verified_maximum_source_interval_ns=1_000_000_001))
        self.assertEqual(invalid["classifications"], [])
        self.assertTrue(invalid["errors"])
        wrong_reader = bars.classify_main_bar_runs(
            chain(), [{"event_id": "e", "start_ns": 0, "end_ns": 100_000_000}],
            context(reader_sha256="f" * 64))
        self.assertEqual(wrong_reader["classifications"], [])
        self.assertTrue(wrong_reader["errors"])


if __name__ == "__main__":
    unittest.main()
