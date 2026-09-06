#!/usr/bin/env python3
"""Compare actual behavior evidence without manufacturing timing regressions."""
from copy import deepcopy
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from bench.encounter_build_comparison import compare_behavior_runs


def witness(ms, value=None):
    return {"capture_ns": 1_000_000_000 + int(ms * 1e6), "image": "frames/000012.png",
            "video_frame_index": 12, "observed": value}


def behavior():
    return {"schema_version": 1, "kind": "firmware_visual_behavior",
            "evidence": {"runtime_identity": {"git_sha": "1111111", "image_id": "123456789", "boot_id": 1},
                         "tooling_source": {"git_sha": "a" * 40},
                         "configuration": {"settings": {"alertPersistenceSeconds": 0}}},
            "reader_method": {"reader.py": "b" * 64},
            "behavior_contract": {"comparison_key": "literal-v1", "source_git_sha": "1111111"},
            "events": [{"event_id": "event-0001", "input_key": [["infDisplayData", "01"], ["infAlertData", "02"]],
                        "target": {"fields": {"secondary": {"allowed": [[]]}}},
                        "observation": {"input_anchor_ns": 1_000_000_000, "target_observed": True,
                                        "first_target_ms": 150.0,
                                        "fields": {"secondary": {"target_observed": True,
                                           "first_target_observation": witness(150),
                                           "counts": {"matching_frames": 20, "different_frames": 0, "unresolved_frames": 0},
                                           "difference_intervals": [], "unresolved_intervals": [],
                                           "post_target_departures": [],
                                           "end_state": {"last_definite_matches_target": True, "unresolved_suffix": []}}}},
                        "findings": [], "coverage": {"selected_recorded_frames": 20, "unobserved_gaps": 0}}]}


def finding(value="Ka34.700", ms=250):
    return {"field": "secondary", "kind": "unwanted_card", "expected": [], "observed": [value],
            "first": witness(ms), "last": witness(ms + 100), "reason": "Unexpected retired card"}


class BuildComparisonTests(unittest.TestCase):
    def test_different_firmware_tooling_and_boot_are_provenance_not_incompatibility(self):
        old, new = behavior(), behavior()
        new["evidence"]["runtime_identity"] = {"git_sha": "2222222", "image_id": "987654321", "boot_id": 99}
        new["evidence"]["tooling_source"] = {"git_sha": "c" * 40}
        new["behavior_contract"]["source_git_sha"] = "2222222"
        untouched = deepcopy((new, old))
        result = compare_behavior_runs(new, old)
        self.assertEqual((new, old), untouched)
        self.assertEqual(result["status"], "COMPARED")
        self.assertEqual(result["summary"]["changed_events"], 0)
        self.assertEqual(result["current"]["runtime_identity"], new["evidence"]["runtime_identity"])
        self.assertEqual(result["current"]["tooling_source"], new["evidence"]["tooling_source"])

    def test_mismatched_inputs_settings_targets_reader_and_contract_do_not_produce_comparison(self):
        mutations = [
            (lambda s: s["events"][0]["input_key"].reverse(), "ordered authored inputs"),
            (lambda s: s["events"].append({**deepcopy(s["events"][0]), "event_id": "event-0002"}), "ordered authored inputs"),
            (lambda s: s["evidence"]["configuration"]["settings"].update(alertPersistenceSeconds=2), "effective settings"),
            (lambda s: s["reader_method"].update({"reader.py": "d" * 64}), "reader method"),
            (lambda s: s["behavior_contract"].update(comparison_key="different-rule"), "comparison rules"),
            (lambda s: s["events"][0]["target"]["fields"]["secondary"].update(allowed=[["Ka"]]), "expected targets"),
        ]
        for mutate, reason in mutations:
            with self.subTest(reason=reason):
                old, new = behavior(), behavior()
                mutate(new)
                result = compare_behavior_runs(new, old)
                self.assertFalse(result["compatible"])
                self.assertEqual(result["events"], [])
                self.assertTrue(any(reason in entry for entry in result["reasons"]))

    def test_timing_above_100_ms_is_measurement_without_failure_or_regression(self):
        old, new = behavior(), behavior()
        new["events"][0]["observation"]["first_target_ms"] = 230.0
        new["events"][0]["observation"]["fields"]["secondary"]["first_target_observation"] = witness(230)
        result = compare_behavior_runs(new, old)
        event = result["events"][0]
        self.assertEqual(event["appearance"], {"baseline_ms": 150, "current_ms": 230, "difference_ms": 80})
        self.assertTrue(event["timing_changed"])
        self.assertFalse(event["content_changed"])
        self.assertNotIn("verdict", event)
        self.assertNotIn("regression", event)
        self.assertEqual(result["status"], "COMPARED")

    def test_findings_compare_literal_and_kind_while_occurrence_times_stay_visible(self):
        old, new = behavior(), behavior()
        old["events"][0]["findings"] = [finding()]
        new["events"][0]["findings"] = [finding(ms=1000)]
        result = compare_behavior_runs(new, old)
        event = result["events"][0]
        self.assertFalse(event["content_changed"])
        self.assertTrue(event["timing_changed"])
        self.assertEqual(event["baseline"]["findings"][0]["first_ms"], 250)
        self.assertEqual(event["current"]["findings"][0]["first_ms"], 1000)
        new["events"][0]["findings"] = [finding("K24.150")]
        changed = compare_behavior_runs(new, old)["events"][0]
        self.assertEqual(changed["newly_observed_findings"][0]["observed"], ["K24.150"])
        self.assertEqual(changed["previously_observed_findings_absent"][0]["observed"], ["Ka34.700"])

    def test_absent_discrepancy_with_unknown_suffix_never_becomes_a_repair(self):
        old, new = behavior(), behavior()
        old["events"][0]["findings"] = [finding()]
        observation = new["events"][0]["observation"]
        observation.update(target_observed=False, first_target_ms=None)
        field = observation["fields"]["secondary"]
        field["counts"] = {"matching_frames": 0, "different_frames": 0, "unresolved_frames": 20}
        field["first_target_observation"] = None
        field["end_state"] = {"last_definite_matches_target": None, "unresolved_suffix": [{"frame_count": 20}]}
        new["events"][0]["coverage"] = {"selected_recorded_frames": 20, "unobserved_gaps": 2}
        result = compare_behavior_runs(new, old)
        event = result["events"][0]
        self.assertTrue(event["uncertainty_changed"])
        self.assertFalse(event["current"]["target_observed"])
        self.assertIsNone(event["appearance"]["difference_ms"])
        self.assertEqual(event["current"]["fields"]["secondary"]["unresolved_suffix_frames"], 20)
        self.assertEqual(event["current"]["coverage"]["unobserved_gaps"], 2)
        self.assertNotIn("fixed", event)
        self.assertNotIn("passed", event)

    def test_acquisition_differences_are_not_promoted_into_classified_findings(self):
        old, new = behavior(), behavior()
        new["events"][0]["observation"]["fields"]["secondary"]["difference_intervals"] = [
            {"first": witness(20, ["old card"]), "last": witness(30, ["old card"]), "frame_count": 2}]
        result = compare_behavior_runs(new, old)
        self.assertFalse(result["events"][0]["content_changed"])
        self.assertEqual(result["events"][0]["newly_observed_findings"], [])

    def test_different_run_clocks_do_not_create_coverage_or_timing_changes(self):
        old = behavior()
        old["events"][0]["findings"] = [finding()]
        old["events"][0]["coverage"].update(start_ns=1_000_000_000, end_ns=2_000_000_000,
                                           first_read_capture_ns=1_010_000_000, last_read_capture_ns=1_990_000_000)
        new = deepcopy(old)
        observation = new["events"][0]["observation"]
        observation["input_anchor_ns"] += 10_000_000_000
        observation["fields"]["secondary"]["first_target_observation"]["capture_ns"] += 10_000_000_000
        for key in ("first", "last"):
            new["events"][0]["findings"][0][key]["capture_ns"] += 10_000_000_000
        for key in ("start_ns", "end_ns", "first_read_capture_ns", "last_read_capture_ns"):
            new["events"][0]["coverage"][key] += 10_000_000_000
        result = compare_behavior_runs(new, old)
        self.assertFalse(result["events"][0]["changed"])

    def test_witness_paths_are_relative_to_named_run_and_unsafe_links_are_omitted(self):
        for path in ("../secret.png", "/private/secret.png", "https://example.com/image.png", "frames/%2e%2e/x.png"):
            new = behavior()
            f = finding()
            f["first"]["image"] = path
            new["events"][0]["findings"] = [f]
            result = compare_behavior_runs(new, behavior())["events"][0]
            observed = result["current"]["findings"][0]
            self.assertNotIn("image", observed["first"])
            self.assertEqual(observed["last"]["image"], "frames/000012.png")
            self.assertEqual(observed["last"]["run"], "current")

    def test_missing_or_nonfinite_measurements_are_incompatible_not_an_empty_success(self):
        for mutate in (
            lambda s: s.update(events=[]),
            lambda s: s["evidence"].update(configuration={}),
            lambda s: s.update(reader_method={}),
            lambda s: s["events"][0]["observation"].update(first_target_ms=float("nan")),
            lambda s: s["events"][0].pop("findings"),
            lambda s: s["events"][0]["observation"].update(fields={}),
            lambda s: s["events"][0]["observation"]["fields"]["secondary"].update(counts={}),
            lambda s: s["events"][0]["observation"]["fields"]["secondary"].update(end_state=None),
        ):
            new = behavior()
            mutate(new)
            result = compare_behavior_runs(new, behavior())
            self.assertEqual(result["status"], "INCOMPATIBLE")
            self.assertTrue(result["reasons"])
            self.assertEqual(result["events"], [])


if __name__ == "__main__":
    unittest.main()
