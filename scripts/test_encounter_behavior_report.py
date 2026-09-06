#!/usr/bin/env python3
"""The user-facing report preserves real distinctions and executable evidence."""
import copy
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from bench.encounter_behavior_report import write_behavior_report
from bench.encounter_observation import summarize_event_observations
from test_encounter_observation import event, span


def report_fixture():
    measured = event([span(1, value=[], field="secondary"),
                      span(2, "DIFFERENCE", ["Ka34.700"], last=4, field="secondary"),
                      span(5, "UNRESOLVED", field="secondary")], first=1)
    observed = summarize_event_observations(measured)
    wrong = observed["fields"]["secondary"]["latest_contrary_observation"]
    return {"schema_version": 1, "kind": "firmware_visual_behavior", "result": "DIFFERENCES_FOUND",
            "evidence": {"runtime_identity": {"git_sha": "1234567", "image_id": "abcdef123", "boot_id": 8},
                         "configuration": {"persistence": 0}, "tooling_source": {"git_sha": "9876543"}},
            "reader_method": {"version": 9}, "summary": {"events": 1, "targets_observed": 1,
                "events_with_findings": 1, "unresolved_field_observations": 1},
            "events": [{"event_id": "event-1", "start_ns": 0, "end_ns": 100_000_000,
                "input_key": "sole K24.150", "target": {"fields": {"secondary": {"allowed": [[]]}}},
                "observation": observed, "observation_spans": measured["observation_spans"],
                "findings": [{"field": "secondary", "kind": "ending_difference", "expected": [],
                              "observed": ["Ka34.700"], "first": wrong, "last": wrong,
                              "reason": "A readable obsolete card remains at the final definite observation.",
                              "rule_ids": ["retired_card"]}]}],
            "samples_index": [{"frame_index": n, "capture_ns": n * 5_000_000,
                               "image": f"frames/{n:06d}.png", "event_id": "event-1", "video_seconds": n / 200}
                              for n in (1, 2, 4, 5)],
            "behavior_contract": {"status": "VERIFIED", "rules": {"retired_card": {
                "status": "VERIFIED", "statement": "A vanished alert can enter grace.",
                "repair_direction": "Prevent grace at zero and verify the physical clear.",
                "locations": [{"path": "src/display_cards.cpp", "line_start": 107,
                    "url": "https://github.com/v1simple/v1simple/blob/1234567/src/display_cards.cpp#L107",
                    "excerpt": "if (previous) retire();"}]}}}}


def embedded(page):
    return json.loads(re.search(r'<script id="behavior-data" type="application/json">(.*?)</script>', page, re.S).group(1))


class EncounterBehaviorReportTests(unittest.TestCase):
    def test_report_keeps_real_observations_expected_values_identity_and_unknown_suffix_distinct(self):
        result = report_fixture()
        unchanged = copy.deepcopy(result)
        with tempfile.TemporaryDirectory() as folder:
            path = write_behavior_report(Path(folder), result)
            page = path.read_text()
            payload = embedded(page)
        self.assertEqual(result, unchanged)
        self.assertEqual(payload["evidence"]["runtime_identity"]["git_sha"], "1234567")
        self.assertEqual(payload["evidence"]["tooling_source"]["git_sha"], "9876543")
        item = payload["events"][0]
        self.assertEqual(item["findings"][0]["expected"], [])
        self.assertEqual(item["findings"][0]["observed"], ["Ka34.700"])
        secondary = item["observation"]["fields"]["secondary"]
        self.assertTrue(secondary["target_observed"])
        self.assertEqual(secondary["post_target_different_frames"], 3)
        self.assertEqual(secondary["end_state"]["unresolved_suffix"][0]["frame_count"], 1)
        self.assertEqual(item["observation_spans"][1]["observed"]["secondary"]["value"], ["Ka34.700"])
        self.assertEqual(payload["samples_index"][-1]["video_seconds"], .025)
        self.assertIn('Retained original witnesses', page)
        self.assertIn('src="original.mov"', page)
        self.assertIn('expected content', page)

    def test_observed_text_cannot_escape_json_or_become_html(self):
        result = report_fixture()
        attack = '</script><script>alert("unexpected")</script>&<img onerror="attack()">'
        result["events"][0]["findings"][0]["observed"] = attack
        with tempfile.TemporaryDirectory() as folder:
            page = write_behavior_report(Path(folder) / "specific.html", result).read_text()
        self.assertNotIn(attack, page)
        self.assertEqual(embedded(page)["events"][0]["findings"][0]["observed"], attack)
        self.assertEqual(page.count('<script>'), 1)

    def test_report_omits_raw_image_profiles_but_keeps_every_uncertainty_interval(self):
        result = report_fixture()
        reading = result["events"][0]["observation_spans"][0]["observed"]["main_arrows"]
        reading["profile"] = {"large_unneeded_profile": [1] * 10000}
        with tempfile.TemporaryDirectory() as folder:
            page = write_behavior_report(Path(folder), result).read_text()
        self.assertNotIn('large_unneeded_profile', page)
        item = embedded(page)["events"][0]
        self.assertEqual(len(item["observation"]["fields"]["secondary"]["unresolved_intervals"]), 1)

    def test_missing_events_and_metadata_produce_an_explicit_empty_report(self):
        with tempfile.TemporaryDirectory() as folder:
            page = write_behavior_report(Path(folder), {"errors": ["No camera evidence"]}).read_text()
        self.assertEqual(embedded(page)["events"], [])
        self.assertEqual(embedded(page)["errors"], ["No camera evidence"])
        self.assertIn('No event observations', page)

    @unittest.skipUnless(shutil.which("node"), "Node is required for the report script syntax check")
    def test_user_facing_javascript_is_valid(self):
        with tempfile.TemporaryDirectory() as folder:
            page = write_behavior_report(Path(folder), report_fixture()).read_text()
        script = re.search(r'<script>(.*?)</script>', page, re.S).group(1)
        completed = subprocess.run([shutil.which("node"), "--check"], input=script,
                                   text=True, capture_output=True)
        self.assertEqual(completed.returncode, 0, completed.stderr)


if __name__ == "__main__":
    unittest.main()
