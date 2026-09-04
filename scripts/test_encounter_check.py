#!/usr/bin/env python3
"""Trust boundaries for sampled encounter selection, decoding and reporting."""
from __future__ import annotations

import json
import hashlib
from pathlib import Path
import shutil
import re
import subprocess
import sys
import tempfile
import types
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent / "bench"))
import encounter_check as check
import encounter_configuration as configuration
import test_counter_check as existing_counter_tests


def inputs():
    stimulus = [dict(requestedHostMonotonicNs=1_000_000_000 + i * 1_000_000_000,
                     notifications=[dict(kind="display_frame", bytesHex="a" if i < 3 else "b")])
                for i in range(10)]
    rows = [dict(host_capture_ns=1_000_000_000 + i * 10_000_000,
                 duration_ns=10_000_000, frame_seq=i + 1) for i in range(1000)]
    return stimulus, rows


def comparison(status="MATCH"):
    return dict(checks={field: dict(status=status, reason="") for field in check.FIELDS})


class EncounterCheckTests(unittest.TestCase):
    def configuration_records(self, snapshots=None):
        snapshots = snapshots or [dict(), dict(), dict()]
        result = [dict(event="serial_reset_requested", host_monotonic_ns=1),
                  dict(event="serial_reset_completed", host_monotonic_ns=2),
                  dict(event="serial_boundary_established", host_monotonic_ns=3,
                       runtime_identity={"boot_id":7}, reset_anchored=True)]
        for index, changes in enumerate(snapshots):
            fields = dict(bootId=7, uptimeMs=1000 + index * 1000, revision=2, activeSlot=1,
                          stealthEnabled=0, priorityArrowOnly=1, alertPersistenceSeconds=0)
            fields.update(changes)
            result.append(dict(event="serial_receive", host_monotonic_ns=(index + 1) * 1_000_000_000,
                               line="CFG " + " ".join(f"{k}={v}" for k,v in fields.items())))
        return result

    def test_recorded_configuration_requires_unchanged_revision_surrounding_samples(self):
        source = configuration.recorded_snapshots(self.configuration_records(), {"boot_id":7})
        samples = [dict(capture_ns=1_100_000_000), dict(capture_ns=2_800_000_000)]
        result = configuration.configuration_for_samples(source, samples)
        self.assertEqual(result["status"], "verified")
        self.assertEqual(result["settings"], dict(stealthEnabled=False, priorityArrowOnly=True, alertPersistenceSeconds=0))
        self.assertEqual(result["active_slot"],1)
        self.assertEqual(result["snapshot_count"],3)
        for timestamp in (999_999_999,3_000_000_001):
            self.assertEqual(configuration.configuration_for_samples(source,[dict(capture_ns=timestamp)])["status"],"unavailable")

    def test_configuration_changed_back_between_reports_remains_unknown(self):
        source = configuration.recorded_snapshots(self.configuration_records([{}, {"revision":4}, {"revision":4}]), {"boot_id":7})
        result = configuration.configuration_for_samples(source,[dict(capture_ns=1_500_000_000),dict(capture_ns=2_500_000_000)])
        self.assertEqual(result["status"],"unavailable")
        self.assertIn("changed",result["reason"])

    def test_malformed_wrong_boot_and_inconsistent_configuration_are_refused(self):
        for changes in ({"bootId":8},{"revision":4294967295},{"stealthEnabled":2},
                        {"activeSlot":3},{"alertPersistenceSeconds":6},{"uptimeMs":1},
                        {"revision":1},{"stealthEnabled":1}):
            with self.subTest(changes=changes):
                source = configuration.recorded_snapshots(self.configuration_records([{}, changes]), {"boot_id":7})
                self.assertEqual(source["status"],"unavailable")
        for line in ("CFG", "CFG bootId=7", self.configuration_records()[3]["line"]+" revision=2"):
            with self.assertRaises(ValueError):
                configuration.parse_snapshot(line)
        self.assertEqual(configuration.recorded_snapshots([], {"boot_id":7})["status"],"unavailable")

    def test_buffered_old_serial_records_do_not_prove_future_configuration(self):
        records = self.configuration_records([{"uptimeMs":10_000}, {"uptimeMs":11_000}])
        records[3]["host_monotonic_ns"] = 20_000_000_000
        records[4]["host_monotonic_ns"] = 22_000_000_000
        source = configuration.recorded_snapshots(records,{"boot_id":7})
        self.assertEqual(source["status"],"available")
        # Same revision in old buffered messages brackets receipt at 21 s,
        # but neither message was emitted after this image.
        result = configuration.configuration_for_samples(source,[dict(capture_ns=21_000_000_000)])
        self.assertEqual(result["status"],"unavailable")
        self.assertEqual(configuration.recorded_snapshots(records[3:],{"boot_id":7})["status"],"unavailable")
        records[2]["reset_anchored"] = False
        self.assertEqual(configuration.recorded_snapshots(records,{"boot_id":7})["status"],"unavailable")

    def bound_run(self):
        fixture = existing_counter_tests.CounterCheckTests()
        fixture.setUp()
        self.addCleanup(fixture.doCleanups)
        timing = dict(status="verified", timestamp_error_count=0, missing_encoded_frame_count=0,
                      extra_encoded_frame_count=0, duration_mismatch_count=0,
                      written_frame_count=3, encoded_frame_count=3, source_frame_count=3)
        fixture.write(fixture.camera/"video_timing_verification.json", timing)
        fixture.bind()
        return fixture, timing

    def test_altered_video_is_refused_before_reading(self):
        fixture, _ = self.bound_run()
        self.assertEqual(len(check.load_run(fixture.run)["rows"]),3)
        video = fixture.camera/"evidence_exp50.mov"
        video.write_bytes(video.read_bytes()+b"different recording")
        with self.assertRaises(RuntimeError):
            check.load_run(fixture.run)

    def test_configuration_uses_owned_timeline_and_refuses_modified_bytes(self):
        fixture, _ = self.bound_run()
        timeline = fixture.run/"bench_timeline.ndjson"
        timeline.write_text("".join(json.dumps(r)+"\n" for r in self.configuration_records()))
        window = json.loads((fixture.run/"window_result.json").read_text())
        window["runtime_identity"] = {"boot_id":7}
        window["artifacts"]["bench_timeline"] = dict(path=timeline.name,size_bytes=timeline.stat().st_size,
                                                    sha256=hashlib.sha256(timeline.read_bytes()).hexdigest())
        fixture.write(fixture.run/"window_result.json",window)
        data = check.load_run(fixture.run)
        self.assertEqual(data["recorded_configuration"]["status"],"available")
        self.assertEqual(data["identity"]["configuration_timeline_sha256"],window["artifacts"]["bench_timeline"]["sha256"])
        timeline.write_text(timeline.read_text().replace("stealthEnabled=0","stealthEnabled=1"))
        self.assertEqual(check.load_run(fixture.run)["recorded_configuration"]["status"],"unavailable")

    def test_unverified_or_incomplete_timing_never_admits_frames(self):
        for changes in ({"status":"unverified"},{"timestamp_error_count":1},
                        {"encoded_frame_count":2},{"duration_mismatch_count":1}):
            with self.subTest(changes=changes):
                fixture,timing = self.bound_run()
                fixture.write(fixture.camera/"video_timing_verification.json",{**timing,**changes})
                fixture.bind()
                with self.assertRaises(ValueError):
                    check.load_run(fixture.run)

    def test_selection_tracks_changed_packet_content_with_unchanged_count(self):
        stimulus, rows = inputs()
        selected = check.select_samples(stimulus, rows, [(0, 9)], 2)
        probes = [s for s in selected if s["role"] == "transition"]
        self.assertEqual([s["requested_offset_seconds"] for s in probes], [2.95, 3.05, 3.15, 3.35])
        midpoints = [s["requested_offset_seconds"] for s in selected if "packet-state midpoint" in s["selection_reasons"]]
        self.assertEqual(midpoints, [1.5, 6])

    def test_long_unchanging_state_still_has_regular_observations(self):
        stimulus, rows = inputs()
        for item in stimulus:
            item["notifications"][0]["bytesHex"] = "unchanged"
        samples = check.select_samples(stimulus, rows, [(0, 9)], 2)
        times = [0, *[s["requested_offset_seconds"] for s in samples], 9]
        self.assertLessEqual(max(b-a for a,b in zip(times,times[1:])), 2)
        self.assertFalse(any(s["role"] == "transition" for s in samples))

    def test_subsecond_cadence_covers_start_of_short_window(self):
        stimulus, rows = inputs()
        samples = check.select_samples(stimulus, rows, [(3,3.1)],.005)
        regular = [s["requested_offset_seconds"] for s in samples if "regular hold" in s["selection_reasons"]]
        self.assertEqual(len(regular),20)
        self.assertAlmostEqual(regular[0],3.0025)
        self.assertAlmostEqual(regular[-1],3.0975)

    def test_capture_gap_retains_required_request(self):
        stimulus, rows = inputs()
        rows = [r for r in rows if not 1_300_000_000 < r["host_capture_ns"] < 1_700_000_000]
        samples = check.select_samples(stimulus, rows, [(0, 9)], 2)
        missing = next(s for s in samples if s["requested_offset_seconds"] == .5)
        self.assertIn("selection_error", missing)

    def test_bad_range_and_unbounded_selection_refused(self):
        stimulus, rows = inputs()
        for ranges, cadence in [([(0, 11)], 2), ([(1, 0)], 2), ([(0, 9)], 0), ([(0, 9)], float("nan")), ([(0, 9)], .0001)]:
            with self.subTest(ranges=ranges,cadence=cadence), self.assertRaises(ValueError):
                check.select_samples(stimulus, rows, ranges, cadence)

    def test_mismatch_survives_unknowns_and_joint_conflict_survives_field_matches(self):
        a = dict(role="held", comparison=comparison())
        b = dict(role="held", comparison=comparison("UNRESOLVED"))
        a["comparison"]["checks"]["secondary"]["status"] = "DIFFERENCE"
        verdict, counts = check.summarize([a,b], ["later decode failed"])
        self.assertEqual(verdict, "FAIL")
        self.assertEqual(counts["required"], 14)
        self.assertEqual(counts["fields"]["UNRESOLVED"], 7)
        a["comparison"] = comparison()
        a["comparison"]["joint_state"] = dict(status="DIFFERENCE")
        self.assertEqual(check.summarize([a], [])[0], "FAIL")

    def test_transition_difference_has_no_automatic_failure_deadline(self):
        sample = dict(role="transition", comparison=comparison("TRANSITION_DIFFERENCE"))
        self.assertEqual(check.summarize([sample], [])[0], "INCONCLUSIVE")
        for status in ("TRANSITION_DIFFERENCE", "PREVIOUS_INPUT_STATE", "UNRESOLVED"):
            sample["comparison"] = comparison()
            sample["comparison"]["joint_state"] = dict(status=status)
            self.assertEqual(check.summarize([sample], [])[0], "INCONCLUSIVE")

    def test_configuration_requires_same_boot_and_full_observation_coverage(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary)/"settings.json"
            identity = dict(window_result_sha256="a"*64,runtime_identity=dict(boot_id=7,image_id="b"))
            settings = {**identity,"status":"verified","basis":"independent retained settings readback",
                        "coverage":dict(start_capture_ns=100,end_capture_ns=200),"settings":{"stealthEnabled":False}}
            path.write_text(json.dumps(settings))
            self.assertEqual(check.configuration_for(path,identity,[dict(capture_ns=150)])["settings"],settings["settings"])
            with self.assertRaises(ValueError):
                check.configuration_for(path,identity,[dict(capture_ns=201)])
            settings["runtime_identity"]["boot_id"] = 8
            path.write_text(json.dumps(settings))
            with self.assertRaises(ValueError):
                check.configuration_for(path,{**identity,"runtime_identity":{"boot_id":7,"image_id":"b"}},[dict(capture_ns=150)])

    def test_interrupted_decoder_keeps_all_selected_checks_and_frozen_selection(self):
        stimulus, rows = inputs()
        data = dict(stimulus=stimulus, rows=rows, identity={}, timing={}, timeline={},
                    video=Path("unused.mov"), width=2, height=2, registration={})
        selected_count = len(check.select_samples(stimulus, rows, [(0,9)],2))
        calls = []
        with tempfile.TemporaryDirectory() as temporary:
            out = Path(temporary)
            def stream(_video,indices,_width,_height):
                yield min(indices), bytes(12)
                raise ValueError("deliberate interrupted decode")
            def reader(rgb,width,height,registration):
                calls.append((rgb,width,height,registration))
                self.assertTrue((out/"selection.json").is_file())
                return check.unresolved("control")
            module = types.SimpleNamespace(observe=reader)
            with patch.object(check,"load_run",return_value=data), patch.object(check,"stream_frames",side_effect=stream), \
                 patch.object(check,"encounter_expectation_at",return_value={}), \
                 patch.object(check,"compare_sample",return_value=comparison("DIFFERENCE")), \
                 patch.dict(sys.modules,{"encounter_reader":module}):
                result = check.analyze(Path("unused"),out,[(0,9)],2)
            self.assertEqual(len(calls),1)
            self.assertEqual(result["result"],"FAIL")
            self.assertEqual(result["counts"]["required"],selected_count*7)
            self.assertEqual(result["counts"]["fields"]["UNRESOLVED"],(selected_count-1)*7)
            self.assertEqual(result["coverage"]["unique_frames"],1)
            self.assertEqual(result["coverage"]["selected_unique_frames"],selected_count)
            self.assertGreater(result["coverage"]["regions"][0]["maximum_unobserved_gap_seconds"],8)
            self.assertIn("interrupted decode",result["errors"][0])
            self.assertTrue((out/"report.html").is_file())
            if shutil.which("node"):
                script = re.search(r"<script>(.*?)</script>",(out/"report.html").read_text(),re.S).group(1)
                subprocess.run([shutil.which("node"),"--check"],input=script,text=True,check=True,capture_output=True)

    @unittest.skipUnless(shutil.which("ffmpeg"), "ffmpeg unavailable")
    def test_many_selected_frames_decode_without_flat_expression_depth_failure(self):
        with tempfile.TemporaryDirectory() as temporary:
            video = Path(temporary)/"frames.mkv"
            subprocess.run([shutil.which("ffmpeg"),"-hide_banner","-loglevel","error","-f","lavfi","-i",
                            "color=orange:size=4x4:rate=200","-frames:v","400","-c:v","ffv1",str(video)],check=True)
            frames = list(check.stream_frames(video,list(range(0,400,2)),4,4))
            self.assertEqual([index for index,_ in frames],list(range(0,400,2)))
            self.assertTrue(all(len(pixels)==48 for _,pixels in frames))


if __name__ == "__main__":
    unittest.main()
