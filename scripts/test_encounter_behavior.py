#!/usr/bin/env python3
"""A useful behavior report must separate acquisition, contradiction and unknowns."""
from copy import deepcopy
from pathlib import Path
import gzip
import json
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent / "bench"))
from encounter_behavior import analyze_behavior, event_findings, summarize
from encounter_observation import summarize_event_observations
from test_encounter_sequence import sequence
from test_encounter_expectation import literals, alert, recording


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
            data = dict(identity={"runtime_identity": {"git_sha": "ee6b401", "image_id": "example", "boot_id": 1}},
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
