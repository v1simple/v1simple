#!/usr/bin/env python3
"""A useful behavior report must separate acquisition, contradiction and unknowns."""
from copy import deepcopy
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import gzip
import json
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent / "bench"))
from encounter_behavior import analyze_behavior, event_findings, summarize
from encounter_behavior_report import interval_coverage
from encounter_observation import summarize_event_observations
from test_encounter_sequence import sequence, camera
from test_encounter_expectation import literals, alert, recording
from encounter_expectation import build_encounter_timeline


def measured(times, values, **kwargs):
    result, *_ = sequence(times, values, **kwargs)
    assert not result["errors"], result["errors"]
    event = result["events"][0]
    obs = summarize_event_observations(event)
    findings = event_findings(event, obs, {"field_rule_ids": {"secondary": ["retired_card"]}})
    return event, {"event_id": event["event_id"], "observation": obs, "findings": findings,
                   "coverage": event["coverage"], "unresolved_frames": sum(
                       s["frame_count"] for s in event["observation_spans"] if s["judgment"].get("unresolved_fields"))}


class BehaviorTests(unittest.TestCase):
    def test_producer_comparison_includes_new_persistence_ending_content(self):
        inputs = recording([
            ([alert()], [6, 6, 1, 0x24, 0x24, 12, 12, 0x40]),
            ([], [56, 56, 0, 0, 0, 12, 12, 0x40]),
            ([alert("ka", 34700)], [6, 6, 1, 0x22, 0x22, 12, 12, 0x40]),
        ])
        inputs[0]["samples"][2]["offsetSeconds"] = 9
        inputs[1][2].update(replayOffsetSeconds=9, requestedHostMonotonicNs=10_000_000_000)
        for delivery in inputs[2]:
            if delivery["stimulusSequence"] == 3:
                for key in ("hostMonotonicNs", "attemptedHostMonotonicNs"):
                    if key in delivery:
                        delivery[key] += 7_000_000_000
        timeline = build_encounter_timeline(*inputs)
        rows = camera([1.03, 2.03, 2.035, 9.99, 10.03])
        for index, row in enumerate(rows):
            row.update(video_pts_value=index, video_pts_timescale=200)
        held = literals(counter_glyph="L", primary_frequency="24.150", active_bands=[], main_arrows=[], main_bars=0)
        cleared = deepcopy(held)
        cleared["primary_frequency"]["value"] = "--.---"
        final = literals(primary_frequency="34.700", active_bands=["Ka"])
        results = {}
        with tempfile.TemporaryDirectory() as folder:
            for name, values in (
                ("baseline", [literals(), held, cleared, cleared, final]),
                ("control", [literals(), held, cleared, cleared, final]),
                ("held", [literals(), held, held, held, final]),
            ):
                run, out = Path(folder) / name / "run", Path(folder) / name / "analysis"
                run.mkdir(parents=True)
                out.mkdir()
                (run / "window_result.json").write_text("{}")
                video = run / "original.mov"
                video.write_bytes(b"synthetic acquisition fixture")
                identity = {"runtime_identity": {"git_sha": "fixture", "image_id": "fixture", "boot_id": 1},
                            "capture_id": name, "camera_artifacts": {},
                            **{key: "0" * 64 for key in ("capture_manifest_sha256", "window_result_sha256",
                                "stimulus_sha256", "delivery_sha256", "scenario_sha256")}}
                data = dict(identity=identity, stimulus=inputs[1], timeline=timeline, rows=rows,
                            source_records=rows, timing={}, video=video, width=1, height=1,
                            registration={}, camera_name="fixture", camera_profile={})
                with patch("encounter_check.load_run", return_value=data), \
                     patch("encounter_behavior.behavior_contract", return_value={
                         "comparison_key": "fixture", "rules": {}, "field_rule_ids": {}}), \
                     patch("encounter_behavior.configuration_for_samples", return_value={"status": "verified", "settings": {
                         "stealthEnabled": False, "priorityArrowOnly": False, "alertPersistenceSeconds": 2}}), \
                     patch("encounter_reader.prepare_reader", return_value={"ocr_available": True}), \
                     patch("encounter_runtime_probe.probe_ocr_runtime", return_value={"status": "operational"}), \
                     patch("encounter_qualification.verify_qualification", return_value={"status": "QUALIFIED"}), \
                     patch("encounter_check.stream_frames", return_value=iter(
                         (i, bytes([i, 0, 0])) for i in range(len(rows)))), \
                     patch("encounter_reader.observe", side_effect=[{"fields": deepcopy(value)} for value in values]):
                    results[name] = analyze_behavior(run, out, compare_to=(
                        Path(folder) / "baseline/analysis/result.json" if name != "baseline" else None))
                self.assertEqual(results[name]["errors"], [])
            self.assertEqual(results["baseline"]["result"], "NO_DIFFERENCES_OBSERVED")
            self.assertEqual(results["control"]["comparison"]["summary"]["changed_events"], 0)
            changed = results["held"]
            self.assertEqual(changed["result"], "DIFFERENCES_FOUND")
            self.assertEqual(changed["persistence"]["summary"]["findings"], 1)
            self.assertEqual(changed["comparison"]["summary"]["content_changed_events"], 1)
            findings = changed["comparison"]["events"][1]["newly_observed_findings"]
            self.assertEqual([(f["kind"], f["observed"]) for f in findings],
                             [("ending_content", "retained_primary")])
            self.assertEqual(findings[0]["last"]["run"], "current")
            self.assertEqual(findings[0]["last"]["capture_ns"], 9_990_000_000)

    def test_full_interval_counts_keep_other_acquisition_beside_unknown_and_existing_findings(self):
        inputs = recording([([alert()], [6, 6, 1, 0x24, 0x24, 12, 12, 0x40]),
                            ([alert("k", 24150, "SIDE")], [6, 6, 1, 0x44, 0x44, 12, 12, 0x40])])
        side = literals(main_arrows=["side"])
        other = literals(main_arrows=["front", "side"])
        mixed = deepcopy(other)
        mixed["primary_frequency"] = {"state": "unreadable", "reason": "partial frequency"}
        unknown = deepcopy(side)
        unknown["primary_frequency"] = mixed["primary_frequency"]
        measured, *_ = sequence([1.02, 2.012, 2.020, 2.025, 2.030, 2.035, 2.040, 2.045, 2.050, 2.055],
                                 [literals(), literals(), literals(), other, mixed, unknown,
                                  side, unknown, literals(main_arrows=["rear"]), side], inputs=inputs)
        event = measured["events"][1]
        before = deepcopy(event)
        findings = event_findings(event, summarize_event_observations(event), {})
        result = interval_coverage(event)
        self.assertEqual(event, before)
        self.assertEqual(findings, event_findings(event, summarize_event_observations(event), {}))
        self.assertEqual([(f["field"], f["kind"]) for f in findings],
                         [("main_arrows", "departure_after_target")])
        counts = result["counts"]
        self.assertEqual(counts["after_complete_input_frames"], 8)
        self.assertEqual(counts["before_complete_input_frames"], 1)
        self.assertEqual([counts[key] for key in (
            "matching_frames", "previous_input_acquisition_frames", "other_acquisition_frames",
            "unresolved_before_target_frames", "unresolved_after_target_frames", "contrary_after_target_frames")],
            [2, 1, 1, 2, 1, 1])
        self.assertEqual(counts["other_acquisition_observed_frames"], 2)
        self.assertEqual(counts["partly_unresolved_other_acquisition_frames"], 1)
        refs = result["other_acquisition_observations"]
        self.assertEqual([ref["partly_unresolved"] for ref in refs], [False, True])
        for ref in refs:
            self.assertEqual(ref["fields"], ["main_arrows"])
            self.assertEqual(event["observation_spans"][ref["span_index"]]["observed"]["main_arrows"]["value"],
                             ["front", "side"])

    def test_interval_without_target_or_anchor_never_invents_a_hold_or_input_boundary(self):
        wrong = literals(main_arrows=["front", "side"])
        unknown = deepcopy(wrong)
        unknown["primary_frequency"] = {"state": "ambiguous"}
        event, _ = measured([1.02, 1.025], [wrong, unknown])
        coverage = interval_coverage(event)
        self.assertIsNone(coverage["first_target_capture_ns"])
        self.assertEqual(coverage["counts"]["other_acquisition_frames"], 1)
        self.assertEqual(coverage["counts"]["unresolved_before_target_frames"], 1)
        self.assertEqual(coverage["counts"]["unresolved_after_target_frames"], 0)
        event["target_basis"] = {}
        unknown_anchor = interval_coverage(event)
        self.assertEqual(unknown_anchor["counts"]["unanchored_frames"], 2)
        self.assertEqual(unknown_anchor["counts"]["after_complete_input_frames"], 0)
        self.assertEqual(unknown_anchor["other_acquisition_observations"], [])

    def test_input_boundary_inside_retained_span_is_not_interpolated(self):
        event, _ = measured([1.02, 1.025], [literals(), literals()])
        event["target_basis"]["first_complete_target_input_ns"] = 1_022_000_000
        result = interval_coverage(event)
        self.assertEqual(result["counts"]["unanchored_frames"], 2)
        self.assertEqual(result["counts"]["after_complete_input_frames"], 0)

    def test_late_but_complete_target_has_measurement_without_invented_failure(self):
        event, out = measured([1.02, 1.2, 1.5], [literals(main_bars=4), literals(), literals()])
        self.assertEqual(out["findings"], [])
        self.assertEqual(event["first_correct"]["capture_ns"], 1_200_000_000)
        verdict, summary = summarize([out], [], {"status": "QUALIFIED"})
        self.assertEqual(verdict, "NO_DIFFERENCES_OBSERVED")
        self.assertEqual(summary["targets_observed"], 1)
        self.assertEqual(out["observation"]["fields"]["main_bars"]["counts"]["different_frames"], 1)

    def test_remaining_wrong_content_indexes_actual_original_and_source_direction(self):
        event, out = measured([1.02, 2.0, 4.0], [literals(main_bars=4)] * 3)
        self.assertEqual(len(out["findings"]), 1)
        finding = out["findings"][0]
        self.assertEqual((finding["field"], finding["kind"]), ("main_bars", "ending_difference"))
        self.assertEqual(finding["last"]["capture_ns"], 4_000_000_000)
        self.assertIn("not a response-time violation", finding["reason"])
        self.assertEqual(summarize([out], [], {"status": "QUALIFIED"})[0], "DIFFERENCES_FOUND")

    def test_one_definite_departure_is_retained_even_when_it_recovers(self):
        event, out = measured([1.02, 1.025, 1.03], [literals(), literals(primary_frequency="24.200"), literals()])
        self.assertEqual(len(out["findings"]), 1)
        self.assertEqual(out["findings"][0]["kind"], "departure_after_target")
        self.assertEqual(out["findings"][0]["observed"]["value"], "24.200")

    def test_per_field_match_during_mixed_acquisition_does_not_start_a_hold(self):
        first = literals(primary_frequency="24.200")
        mixed = literals(primary_frequency="24.200", main_bars=4)
        event, out = measured([1.02, 1.025, 1.2], [first, mixed, literals()])
        self.assertEqual(out["findings"], [])
        self.assertEqual(out["observation"]["fields"]["main_bars"]["counts"]["different_frames"], 1)
        self.assertEqual(event["first_correct"]["capture_ns"], 1_200_000_000)

    def test_unknown_suffix_does_not_erase_wrong_content_or_claim_recovery(self):
        unknown = literals()
        unknown["main_bars"] = {"state": "unreadable", "reason": "partial repaint"}
        event, out = measured([1.02, 1.025, 1.03], [literals(), literals(main_bars=4), unknown])
        self.assertEqual(out["findings"][0]["kind"], "ending_difference")
        self.assertEqual(out["observation"]["fields"]["main_bars"]["end_state"]["unresolved_suffix"][0]["frame_count"], 1)
        self.assertEqual(summarize([out], [], {"status": "QUALIFIED"})[1]["unresolved_frames"], 1)

    def test_unknown_only_is_not_wrong_content_and_cannot_complete_target(self):
        unknown = literals()
        unknown["main_arrows"] = {"state": "unreadable", "reason": "partial color"}
        event, out = measured([1.02, 1.025], [unknown, unknown])
        self.assertEqual(out["findings"], [])
        verdict, summary = summarize([out], [], {"status": "QUALIFIED"})
        self.assertEqual(verdict, "MEASUREMENT_INCOMPLETE")
        self.assertEqual(summary["targets_observed"], 0)

    def test_unknown_ending_after_only_wrong_acquisition_remains_incomplete(self):
        unknown = literals()
        unknown["main_bars"] = {"state": "unreadable", "reason": "reader refusal"}
        wrong = literals(main_bars=4)
        event, out = measured([1.02, 1.025, 1.03], [wrong, unknown, unknown])
        self.assertIsNone(event["first_correct"])
        self.assertEqual(out["findings"], [])
        field = out["observation"]["fields"]["main_bars"]
        self.assertEqual(field["counts"]["different_frames"], 1)
        self.assertEqual(len(field["end_state"]["unresolved_suffix"]), 1)
        self.assertEqual(summarize([out], [], {"status": "QUALIFIED"})[0], "MEASUREMENT_INCOMPLETE")
        event, out = measured([1.02, 1.025, 1.03], [wrong, unknown, wrong])
        self.assertIsNone(event["first_correct"])
        self.assertEqual(out["findings"][0]["kind"], "ending_difference")
        self.assertEqual(summarize([out], [], {"status": "QUALIFIED"})[0], "DIFFERENCES_FOUND")

    def test_unknown_joint_ending_does_not_promote_initial_mixed_phase(self):
        inputs = recording([([alert()], [6, 0, 1, 0x24, 0, 12, 12, 0x40])])
        mixed = literals(active_bands=[], main_arrows=[])
        unknown = deepcopy(mixed)
        unknown["counter_glyph"] = {"state": "unreadable", "reason": "partial counter"}
        event, out = measured([1.02, 1.025, 1.03], [mixed, unknown, unknown], inputs=inputs)
        self.assertIsNone(event["first_correct"])
        self.assertEqual(out["findings"], [])
        self.assertEqual(summarize([out], [], {"status": "QUALIFIED"})[0], "MEASUREMENT_INCOMPLETE")
        event, out = measured([1.02, 1.025, 1.03], [mixed, unknown, mixed], inputs=inputs)
        self.assertIsNone(event["first_correct"])
        self.assertEqual((out["findings"][0]["field"], out["findings"][0]["kind"]),
                         ("joint_state", "ending_difference"))

    def test_permitted_joint_blink_is_not_a_departure(self):
        inputs = recording([([alert()], [6, 0, 1, 0x24, 0, 12, 12, 0x40])])
        off = literals(active_bands=[], main_arrows=[])
        off["counter_glyph"] = {"state": "absent"}
        event, out = measured([1.02, 1.025, 1.03], [literals(), off, literals()], inputs=inputs)
        self.assertEqual(out["findings"], [])
        mixed = deepcopy(off)
        mixed["counter_glyph"] = literals()["counter_glyph"]
        event, out = measured([1.02, 1.025, 1.03], [literals(), mixed, literals()], inputs=inputs)
        self.assertEqual(out["findings"][0]["field"], "joint_state")
        event, out = measured([1.02, 1.025], [mixed, mixed], inputs=inputs)
        self.assertIsNone(event["first_correct"])
        self.assertEqual(out["findings"][0]["kind"], "ending_difference")
        self.assertEqual(summarize([out], [], {"status": "QUALIFIED"})[0], "DIFFERENCES_FOUND")

    def test_capture_gaps_and_unqualified_reader_never_produce_clean_observation_result(self):
        event, out = measured([1.02, 1.025, 1.03], [literals()] * 3, selected={0, 2})
        self.assertEqual(summarize([out], [], {"status": "QUALIFIED"})[0], "MEASUREMENT_INCOMPLETE")
        self.assertEqual(summarize([out], [], {"status": "REJECTED"})[0], "MEASUREMENT_INCOMPLETE")

    def test_truncated_selection_cannot_call_early_acquisition_an_ending_difference(self):
        event, out = measured([1.02, 1.025, 1.03],
                              [literals(main_bars=4), literals(main_bars=4), literals()], selected={0, 1})
        self.assertEqual(out["findings"], [])
        self.assertEqual(summarize([out], [], {"status": "QUALIFIED"})[0], "MEASUREMENT_INCOMPLETE")
        event, out = measured([1.02, 1.025, 1.03],
                              [literals(), literals(main_bars=4), literals()], selected={0, 1})
        self.assertEqual(out["findings"][0]["kind"], "departure_after_target")

    def test_ordinary_producer_reads_all_frames_retains_exact_witnesses_and_full_raw_readings(self):
        _, _, timeline, rows = sequence([1.02, 1.025, 1.03], [literals()] * 3)
        stimuli = [{"stimulusSequence": s["stimulus_sequence"], "replayOffsetSeconds": i,
                    "requestedHostMonotonicNs": s["stimulus_requested_ns"]} for i, s in enumerate(timeline["states"])]
        for i, row in enumerate(rows):
            row.update(video_pts_value=i, video_pts_timescale=200)
        with tempfile.TemporaryDirectory() as folder:
            run = Path(folder) / "run"
            out = Path(folder) / "report"
            run.mkdir(); out.mkdir()
            video = run / "original.mov"
            video.write_bytes(b"test acquisition stub")
            (run / "window_result.json").write_text("{}")
            identity = {"runtime_identity": {"git_sha": "ee6b401", "image_id": "example", "boot_id": 1},
                        "capture_id": "fixture", "camera_artifacts": {},
                        **{key: "0" * 64 for key in ("capture_manifest_sha256", "window_result_sha256",
                            "stimulus_sha256", "delivery_sha256", "scenario_sha256")}}
            data = dict(identity=identity,
                        stimulus=stimuli, timeline=timeline, source_records=rows, rows=rows, timing={},
                        video=video, width=1, height=1, registration={"fixed": True},
                        camera_name="fixture", camera_profile={})
            with patch("encounter_check.load_run", return_value=data), \
                 patch("encounter_behavior.configuration_for_samples", return_value={"status": "verified", "settings": {
                     "stealthEnabled": False, "priorityArrowOnly": False, "alertPersistenceSeconds": 0}}), \
                 patch("encounter_reader.prepare_reader", return_value={"ocr_available": True}), \
                 patch("encounter_runtime_probe.probe_ocr_runtime", return_value={"status": "operational"}), \
                 patch("encounter_qualification.verify_qualification", return_value={"status": "QUALIFIED"}), \
                 patch("encounter_check.stream_frames", return_value=iter([(i, bytes([i, 0, 0])) for i in range(3)])), \
                 patch("encounter_reader.observe", return_value={"fields": literals()}) as reader:
                result = analyze_behavior(run, out)
                reused_out = Path(folder) / "reused"
                reused_out.mkdir()
                with patch("encounter_reader.observe", side_effect=AssertionError("reuse must not reinterpret pixels")):
                    reused = analyze_behavior(run, reused_out, reuse_readings=out / "result.json")
                self.assertEqual(reused["errors"], [])
                self.assertEqual(reused["summary"], result["summary"])
                self.assertEqual(reused["events"], result["events"])
                self.assertEqual(reused["evidence"]["reused_readings"]["readings_reused"], 3)
                parallel_out = Path(folder) / "parallel"
                parallel_out.mkdir()
                with patch("encounter_frame_workers.ProcessPoolExecutor", side_effect=lambda **kw: ThreadPoolExecutor(kw["max_workers"])), \
                     patch("encounter_frame_workers._read", side_effect=lambda job: (job[0], {"fields": literals()}, None, None)), \
                     patch("encounter_check.stream_frames", return_value=iter([(i, bytes([i, 0, 0])) for i in range(3)])):
                    parallel = analyze_behavior(run, parallel_out, workers=2)
                for key in ("events", "summary", "samples_index", "errors"):
                    self.assertEqual(parallel[key], result[key])
                self.assertEqual(gzip.decompress((parallel_out / "readings.ndjson.gz").read_bytes()),
                                 gzip.decompress((out / "readings.ndjson.gz").read_bytes()))
                def failed_decoder():
                    yield from [(i, bytes([i, 0, 0])) for i in range(3)]
                    raise ValueError("decoder failed after its final image")
                failed_out = Path(folder) / "failed"
                failed_out.mkdir()
                with patch("encounter_check.stream_frames", return_value=failed_decoder()):
                    failed = analyze_behavior(run, failed_out)
                self.assertEqual(failed["result"], "MEASUREMENT_INCOMPLETE")
                self.assertTrue(any("decoder failed" in e for e in failed["errors"]))
            self.assertEqual(result["errors"], [])
            self.assertEqual(result["result"], "NO_DIFFERENCES_OBSERVED")
            self.assertEqual(result["summary"]["read_frames"], 3)
            self.assertEqual(result["summary"]["interval_coverage"]["counts"]["matching_frames"], 3)
            self.assertEqual(result["events"][0]["interval_coverage"]["counts"]["after_complete_input_frames"], 3)
            self.assertEqual([s["frame_index"] for s in result["samples_index"]], [0, 2])
            self.assertEqual(reader.call_count, 6)
            self.assertEqual(reader.call_args.args, (bytes([2, 0, 0]), 1, 1, {"fixed": True}))
            self.assertEqual(len(list(gzip.open(out / "readings.ndjson.gz", "rt"))), 3)
            first = result["events"][0]["observation"]["first_target_observation"]
            self.assertTrue((out / first["image"]).is_file())
            self.assertEqual(len(first["image_sha256"]), 64)
            self.assertTrue((out / "report.html").is_file())
            self.assertNotIn("appearance_deadline", json.dumps(result))


if __name__ == "__main__":
    unittest.main()
