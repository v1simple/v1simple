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

    def test_long_run_configuration_needs_a_fresh_closing_bracket(self):
        opening = self.configuration_records(
            [{"uptimeMs": 1000}, {"uptimeMs": 41000}]
        )
        opening[4]["host_monotonic_ns"] = 42_000_000_000
        sample = [dict(capture_ns=281_500_000_000)]
        source = configuration.recorded_snapshots(opening, {"boot_id":7})
        self.assertEqual(
            configuration.configuration_for_samples(source, sample)["status"],
            "unavailable",
        )

        closing = self.configuration_records(
            [
                {"uptimeMs": 1000},
                {"uptimeMs": 41000},
                {"uptimeMs": 305000},
            ]
        )
        closing[4]["host_monotonic_ns"] = 42_000_000_000
        closing[5]["host_monotonic_ns"] = 306_000_000_000
        verified = configuration.configuration_for_samples(
            configuration.recorded_snapshots(closing, {"boot_id":7}), sample
        )
        self.assertEqual(verified["status"], "verified")

        changed = [dict(record) for record in closing]
        changed[-1] = dict(changed[-1])
        changed[-1]["line"] = changed[-1]["line"].replace(
            "revision=2", "revision=3"
        )
        self.assertEqual(
            configuration.configuration_for_samples(
                configuration.recorded_snapshots(changed, {"boot_id":7}), sample
            )["status"],
            "unavailable",
        )

        late_old = [dict(record) for record in closing]
        late_old[-1] = dict(late_old[-1])
        late_old[-1]["line"] = late_old[-1]["line"].replace(
            "uptimeMs=305000", "uptimeMs=42000"
        )
        self.assertEqual(
            configuration.configuration_for_samples(
                configuration.recorded_snapshots(late_old, {"boot_id":7}), sample
            )["status"],
            "unavailable",
        )

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

    def test_all_frames_uses_source_indices_with_irregular_timing_and_exclusive_end(self):
        stimulus, _ = inputs()
        rows = [dict(host_capture_ns=1_000_000_000 + offset, duration_ns=5_000_000, frame_seq=i + 1)
                for i, offset in enumerate([0, 2_000_000, 4_900_000, 10_500_000, 11_000_000, 20_000_000])]
        # The two closely spaced frames cannot both be selected by a 5 ms grid.
        selected = check.select_all_frames(stimulus, rows, [(0, .02), (.002, .011)])
        self.assertEqual([s["video_frame_index"] for s in selected], [0, 1, 2, 3, 4])
        self.assertEqual([s["capture_ns"] for s in selected], [r["host_capture_ns"] for r in rows[:5]])
        self.assertEqual(len({s["source_frame_seq"] for s in selected}), 5)
        self.assertTrue(all(s["target_capture_ns"] == s["capture_ns"] for s in selected))
        for ranges in ([], [(0, .026)], [(float("nan"), .02)]):
            with self.assertRaises(ValueError):
                check.select_all_frames(stimulus, rows, ranges)
        with patch.object(check, "MAX_SAMPLES", 4), self.assertRaises(ValueError):
            check.select_all_frames(stimulus, rows, [(0, .02)])

    def test_literal_spans_retain_one_frame_wrong_state_unknown_and_source_gaps(self):
        stimulus, rows = inputs()
        samples = check.select_all_frames(stimulus, rows, [(0, .07)])
        for sample, value in zip(samples, [2, 2, 8, 2, None, 2, 2]):
            sample["observed"] = {"fields": {f: dict(state="read", value=0) for f in check.FIELDS}}
            sample["observed"]["fields"]["main_bars"] = dict(
                state="unreadable" if value is None else "read", value=value)
        spans, changes = check.observation_history(samples)
        bars = spans["main_bars"]
        self.assertEqual([span["observed"]["value"] for span in bars], [2, 8, 2, None, 2])
        self.assertEqual([span["frame_count"] for span in bars], [2, 1, 1, 1, 2])
        self.assertEqual(bars[1]["first"], bars[1]["last"])
        self.assertEqual([c["video_frame_index"] for c in changes], [0, 2, 3, 4, 5])
        # Identical readings either side of a dropped source frame must not form one span.
        samples[-1]["source_frame_seq"] += 1
        spans, changes = check.observation_history(samples)
        self.assertEqual([span["frame_count"] for span in spans["main_bars"]][-2:], [1, 1])
        self.assertFalse(changes[-1]["consecutive_with_previous"])
        self.assertEqual(changes[-1]["fields"], {})

    def test_transition_review_keeps_held_requirements_and_every_input_window_frame(self):
        stimulus, rows = inputs()
        held = check.select_samples(stimulus, rows, [(0, 9)], 2)
        samples, windows, held_windows = check.select_transition_review(stimulus, rows, [(0, 9)], 2, 10_000_000)
        self.assertEqual(windows, [(0, .5), (2.95, 3.5)])
        self.assertTrue(held_windows)
        by_target = {s["target_capture_ns"]: s for s in samples}
        for original in held:
            sample = by_target[original["target_capture_ns"]]
            self.assertEqual(sample["role"], original["role"])
            self.assertEqual(sample.get("video_frame_index"), original.get("video_frame_index"))
        indices = {s["video_frame_index"] for s in samples if "video_frame_index" in s}
        self.assertTrue(set(range(50)) <= indices)
        self.assertTrue(set(range(295, 350)) <= indices)
        # Every held point receives a fixed input-only neighborhood.  A target
        # at .5 s therefore includes the source images around 0.394--0.606 s.
        self.assertTrue(set(range(40, 61)) <= indices)
        self.assertEqual(len({s["frame_id"] for s in samples}), len(samples))

    def test_transition_review_does_not_drop_unavailable_required_sample(self):
        stimulus, rows = inputs()
        rows = [r for r in rows if not 1_300_000_000 < r["host_capture_ns"] < 1_700_000_000]
        samples, _, _ = check.select_transition_review(stimulus, rows, [(0, 9)], 2, 10_000_000)
        missing = next(s for s in samples if s["requested_offset_seconds"] == .5)
        self.assertIn("selection_error", missing)
        self.assertNotIn("video_frame_index", missing)

    def test_dense_automatic_review_keeps_complete_recording_above_old_frame_cap(self):
        origin = 1_000_000_000
        rows = [dict(host_capture_ns=origin + index * 5_000_000,
                     duration_ns=5_000_000, frame_seq=index + 1)
                for index in range(22000)]
        stimulus = [dict(requestedHostMonotonicNs=origin + index * 500_000_000,
                         notifications=[dict(kind="display_frame", bytesHex=str(index))])
                    for index in range(220)]
        # Adjacent input windows cover the full 110-second recording. Neither
        # a frame budget nor overlapping held windows may truncate that union.
        samples, _, _ = check.select_transition_review(
            stimulus, rows, [(0, 110)], 2, 20_000_000)
        indices = {sample["video_frame_index"] for sample in samples
                   if "video_frame_index" in sample}
        self.assertEqual(indices, set(range(22000)))
        self.assertLessEqual(len(samples), len(rows) + check.MAX_SAMPLES)

        # Product windows independently exceed the former cap. Every source
        # image through the last complete event still belongs to the review.
        definitions = [
            {"event_id": f"event-{index}",
             "end_ns": origin + 10_000_000 + (index + 1) * 312_000_000,
             "target_basis": {
                 "first_complete_target_input_ns": origin + 10_000_000 + index * 312_000_000}}
            for index in range(350)]
        product, windows = check.select_product_event_windows(
            [], stimulus, rows, [(0, 110)], definitions,
            {"maximum_source_marker_gap_ns": 10_000_000,
             "minimum_post_completion_hold_ns": 312_000_000})
        self.assertEqual(len(windows), 350)
        self.assertEqual([sample["video_frame_index"] for sample in product],
                         list(range(21842)))

    def test_held_context_requires_verified_interval_and_is_frozen_for_every_hold(self):
        stimulus, rows = inputs()
        for value in (None, 0, -1, 1_000_000_001, 1.5):
            with self.subTest(value=value), self.assertRaises(ValueError):
                check.select_transition_review(stimulus, rows, [(0, 9)], 2, value)
        samples, _, held_windows = check.select_transition_review(
            stimulus, rows, [(0, 9)], 2, 10_000_000)
        held = check.select_samples(stimulus, rows, [(0, 9)], 2)
        held = [sample for sample in held if sample["role"] == "held" and "offset_seconds" in sample]
        self.assertEqual(len(held_windows), len(held))
        for sample in held:
            radius = (check.BLINK_PHASE_NS + 10_000_000) / 1e9
            self.assertTrue(any(start <= sample["offset_seconds"] <= end
                                and end - start <= 2 * radius + 2e-9
                                for start, end in held_windows))
        self.assertTrue(any("fixed context around a held checkpoint" in sample["selection_reasons"]
                            for sample in samples if sample["role"] == "transition"))

    def test_held_context_boundary_uses_integer_capture_time_for_its_reason(self):
        origin = 185_778_852_652_625
        center = origin + 6_334_780_708
        boundary = center - 111_000_000
        stimulus = [dict(requestedHostMonotonicNs=origin,
                         notifications=[dict(kind="display_frame", bytesHex="a")])]
        rows = [dict(host_capture_ns=value, duration_ns=5_000_000, frame_seq=index + 1)
                for index, value in enumerate((boundary, center, origin + 6_834_780_708))]
        samples, _, _ = check.select_transition_review(
            stimulus, rows, [(5.834780708, 6.834780708)], 10, 15_000_000)
        selected_boundary = next(sample for sample in samples if sample.get("capture_ns") == boundary)
        self.assertIn("fixed context around a held checkpoint", selected_boundary["selection_reasons"])
        self.assertTrue(all(sample["selection_reasons"] for sample in samples))

    def test_product_windows_use_exact_input_anchor_and_keep_raw_selection_fixed(self):
        stimulus, rows = inputs()
        initial = check.select_samples(stimulus, rows, [(0, 2)], 2)
        definitions = [{"event_id": "event-0001", "end_ns": 1_500_000_000,
                        "target_basis": {"first_complete_target_input_ns": 1_100_000_000}},
                       {"event_id": "outside", "end_ns": 4_000_000_000,
                        "target_basis": {"first_complete_target_input_ns": 3_000_000_000}}]
        policy = {"maximum_source_marker_gap_ns": 10_000_000,
                  "minimum_post_completion_hold_ns": 312_000_000}
        samples, windows = check.select_product_event_windows(
            initial, stimulus, rows, [(0, 2)], definitions, policy)
        self.assertEqual(windows, [{"event_id": "event-0001", "start_ns": 1_090_000_000,
                                    "end_ns": 1_412_000_000, "selected_end_ns": 1_412_000_000,
                                    "closure_context_end_ns": 1_492_000_000,
                                    "anchor_ns": 1_100_000_000, "clipped_by_event_end": False}])
        product = [sample for sample in samples if
                   "every recorded frame in an exact product event window and bounded closure context" in sample["selection_reasons"]]
        self.assertEqual([sample["capture_ns"] for sample in product],
                         list(range(1_090_000_000, 1_500_000_000, 10_000_000)))
        self.assertEqual(len(samples), len({sample["frame_id"] for sample in samples}))

    def test_product_window_preserves_clipped_episode_for_inconclusive_judgment(self):
        stimulus, rows = inputs()
        definitions = [{"event_id": "event-0001", "end_ns": 1_250_000_000,
                        "target_basis": {"first_complete_target_input_ns": 1_100_000_000}}]
        samples, windows = check.select_product_event_windows(
            [], stimulus, rows, [(0, 2)], definitions,
            {"maximum_source_marker_gap_ns": 10_000_000,
             "minimum_post_completion_hold_ns": 312_000_000})
        self.assertTrue(windows[0]["clipped_by_event_end"])
        self.assertEqual(windows[0]["selected_end_ns"], 1_250_000_000)
        self.assertEqual(samples[-1]["capture_ns"], 1_240_000_000)

    def test_product_auxiliary_tail_clips_at_next_input_and_must_fit_declared_range(self):
        stimulus, rows = inputs()
        event = {"event_id": "event-0001", "end_ns": 1_440_000_000,
                 "target_basis": {"first_complete_target_input_ns": 1_100_000_000}}
        policy = {"maximum_source_marker_gap_ns": 10_000_000,
                  "minimum_post_completion_hold_ns": 312_000_000}
        samples, windows = check.select_product_event_windows(
            [], stimulus, rows, [(0, 2)], [event], policy)
        self.assertEqual(windows[0]["end_ns"], 1_412_000_000)
        self.assertEqual(windows[0]["closure_context_end_ns"], 1_440_000_000)
        self.assertEqual(samples[-1]["capture_ns"], 1_430_000_000)
        self.assertEqual(check.product_window_samples(samples, windows), samples)
        # Having the ordinary window but excluding its frozen auxiliary tail
        # cannot silently become a smaller pixel-dependent selection.
        _, windows = check.select_product_event_windows(
            [], stimulus, rows, [(0, 0.42)], [event], policy)
        self.assertEqual(windows, [])

    def test_default_product_scope_refuses_a_missing_derived_event_window(self):
        definitions = [{"event_id": "event-0001"}, {"event_id": "event-0002"}]
        windows = [{"event_id": "event-0001"}]
        with self.assertRaisesRegex(ValueError, "event-0002"):
            check.require_complete_product_windows(definitions, windows)

    def test_internal_temporal_submission_drops_only_fully_decisive_records(self):
        event = {"event_id": "event-0001", "observations": [
            {"video_frame_index": 10, "raw_status": "PREVIOUS"},
            {"video_frame_index": 11, "raw_status": "UNRESOLVED"},
        ]}
        record = lambda index: {"event_id": "event-0001", "classifier_id": "allowed",
                                "video_frame_indices": [index]}
        submitted = check.product_temporal_submissions(
            [record(10), record(11)], [event], {"allowed"})
        self.assertEqual(submitted, [record(11)])

    def test_compact_review_retains_partial_observations_without_changing_measurements(self):
        reading = {"state": "ambiguous", "value": None, "reason": "side faint",
                   "visible_directions": ["front"], "direction_states": {"side": {"state": "faint"}},
                   "arrows": {"private_measurement": [1, 2, 3]}}
        result = {"samples": [{"comparison": comparison(), "observed": {"fields": {"main_arrows": reading}}}]}
        payload = check.review_payload(result)
        retained = payload["samples"][0]["observed"]["fields"]["main_arrows"]
        self.assertEqual(retained["direction_states"], reading["direction_states"])
        self.assertIsNone(retained["value"])
        self.assertNotIn("arrows", retained)
        self.assertIn("arrows", result["samples"][0]["observed"]["fields"]["main_arrows"])

    def test_all_frame_coverage_separates_source_drops_and_unfinished_reader(self):
        stimulus, rows = inputs()
        samples = check.select_all_frames(stimulus, rows, [(0, .035)])
        source_records = [{**r, "status": "written"} for r in rows]
        source_records.append(dict(status="capture_drop", host_capture_ns=1_015_000_000))
        data = dict(stimulus=stimulus, rows=rows, source_records=source_records)
        for sample in samples:
            sample["observed"] = check.unresolved("pixel control")
        region = check.observation_coverage(samples, [(0, .035)], data, True)[0]
        self.assertTrue(region["complete_recorded_frame_coverage"])
        self.assertEqual(region["available_recorded_frames"], 4)
        self.assertEqual(region["unrecorded_source_frames"], 1)
        self.assertEqual(region["first_selected_offset_seconds"], 0)
        self.assertEqual(region["last_selected_offset_seconds"], .03)
        self.assertAlmostEqual(region["maximum_capture_gap_seconds"], .01)
        del samples[-1]["observed"]
        region = check.observation_coverage(samples, [(0, .035)], data, True)[0]
        self.assertFalse(region["complete_recorded_frame_coverage"])
        self.assertEqual(region["unread_recorded_source_frames"], [4])
        self.assertEqual(region["last_observed_offset_seconds"], .02)
        self.assertAlmostEqual(region["maximum_unobserved_gap_seconds"], .015)

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

    def test_configuration_accepts_only_typed_supported_settings(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "settings.json"
            identity = dict(window_result_sha256="a" * 64,
                            runtime_identity=dict(boot_id=7, image_id="b"))
            base = {**identity, "status": "verified", "basis": "independent readback",
                    "coverage": dict(start_capture_ns=100, end_capture_ns=200)}
            valid = [
                {"stealthEnabled": False},
                {"priorityArrowOnly": True},
                {"alertPersistenceSeconds": 0},
                {"stealthEnabled": True, "priorityArrowOnly": False,
                 "alertPersistenceSeconds": 5},
            ]
            for settings in valid:
                with self.subTest(valid=settings):
                    path.write_text(json.dumps({**base, "settings": settings}))
                    self.assertEqual(check.configuration_for(
                        path, identity, [dict(capture_ns=150)])["settings"], settings)
            invalid = [
                {}, {"unknown": False}, {"stealthEnabled": 0},
                {"priorityArrowOnly": "false"}, {"alertPersistenceSeconds": True},
                {"alertPersistenceSeconds": -1}, {"alertPersistenceSeconds": 6},
            ]
            for settings in invalid:
                with self.subTest(invalid=settings):
                    path.write_text(json.dumps({**base, "settings": settings}))
                    with self.assertRaises(ValueError):
                        check.configuration_for(path, identity, [dict(capture_ns=150)])

    def test_default_range_includes_recording_tail_after_final_request(self):
        stimulus = [dict(requestedHostMonotonicNs=1_000_000_000,
                         notifications=[dict(kind="display_frame", bytesHex="a")]),
                    dict(requestedHostMonotonicNs=2_000_000_000,
                         notifications=[dict(kind="display_frame", bytesHex="b")])]
        rows = [dict(host_capture_ns=900_000_000, duration_ns=5_000_000, frame_seq=1),
                dict(host_capture_ns=2_500_000_000, duration_ns=5_000_000, frame_seq=2)]
        self.assertEqual(check.full_camera_range(stimulus, rows), [(-.1, 1.505)])
        selected = check.select_samples(stimulus, rows,
                                        check.full_camera_range(stimulus, rows), 2)
        self.assertLess(check.full_camera_range(stimulus, rows)[0][0], 0)
        self.assertGreater(max(sample["requested_offset_seconds"] for sample in selected), 1)
        self.assertGreater(check.full_camera_range(stimulus, rows)[0][1],
                           (stimulus[-1]["requestedHostMonotonicNs"] -
                            stimulus[0]["requestedHostMonotonicNs"]) / 1e9)

    def test_product_configuration_excludes_diagnostic_preroll_and_tail(self):
        stimulus = [dict(requestedHostMonotonicNs=1_000_000_000),
                    dict(requestedHostMonotonicNs=2_000_000_000)]
        policy = {"maximum_source_marker_gap_ns": 10_000_000,
                  "minimum_post_completion_hold_ns": 312_000_000}
        self.assertEqual(check.product_configuration_scope(stimulus, policy), [
            {"capture_ns": 990_000_000}, {"capture_ns": 2_312_000_000}])
        samples = [dict(capture_ns=value) for value in
                   (900_000_000, 995_000_000, 1_100_000_000, 2_250_000_000, 2_500_000_000)]
        windows = [dict(start_ns=990_000_000, selected_end_ns=1_200_000_000),
                   dict(start_ns=2_200_000_000, selected_end_ns=2_312_000_000)]
        self.assertEqual([sample["capture_ns"] for sample in
                          check.product_window_samples(samples, windows)],
                         [995_000_000, 1_100_000_000, 2_250_000_000])

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

    def test_all_frame_analysis_freezes_complete_selection_and_keeps_transition_observational(self):
        stimulus, rows = inputs()
        data = dict(stimulus=stimulus, rows=rows, identity={}, timing={}, timeline={},
                    video=Path("unused.mov"), width=2, height=2, registration={})
        seen_roles = []
        with tempfile.TemporaryDirectory() as temporary:
            out = Path(temporary)
            def stream(_video, indices, _width, _height):
                frozen = json.loads((out / "selection.json").read_text())
                self.assertEqual(frozen["selection_mode"], "all_recorded_frames")
                self.assertIsNone(frozen["cadence_seconds"])
                self.assertEqual(indices, [0, 1, 2, 3])
                for index in indices:
                    yield index, bytes(12)
            def compare(_requirement, _reading, role):
                seen_roles.append(role)
                return comparison("TRANSITION_DIFFERENCE")
            module = types.SimpleNamespace(observe=lambda *args: check.unresolved("control"))
            with patch.object(check, "load_run", return_value=data), patch.object(check, "stream_frames", side_effect=stream), \
                 patch.object(check, "encounter_expectation_at", return_value={}), \
                 patch.object(check, "compare_sample", side_effect=compare), \
                 patch.dict(sys.modules, {"encounter_reader": module}):
                result = check.analyze(Path("unused"), out, [(0, .04)], 2, transition_only=True, all_frames=True)
            self.assertEqual(seen_roles, ["transition"] * 4)
            self.assertEqual(result["result"], "INCONCLUSIVE")
            self.assertEqual(result["response_deadline"], "not_evaluated")
            self.assertTrue(result["coverage"]["regions"][0]["complete_recorded_frame_coverage"])
            self.assertEqual(result["counts"]["required"], 28)
            self.assertEqual(result["observed_state_spans"]["main_bars"][0]["frame_count"], 4)
            page = (out / "report.html").read_text()
            for text in ("Observed changes", "Per-field observed spans", 'id="scrub"'):
                self.assertIn(text, page)
            if shutil.which("node"):
                script = re.search(r"<script>(.*?)</script>", page, re.S).group(1)
                subprocess.run([shutil.which("node"), "--check"], input=script, text=True, check=True, capture_output=True)

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
