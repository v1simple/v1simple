#!/usr/bin/env python3
"""Instrument fixtures for sampled comparisons, not evidence of DUT defects."""

from __future__ import annotations

import copy
import hashlib
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "bench"))
from visual_compare import FIELDS, _publish_new, compare  # noqa: E402


def digest(text: str) -> str:
    return hashlib.sha256(text.encode()).hexdigest()


def fixture() -> tuple[dict, dict]:
    source = {"run_id": "instrument-fixture", "video_sha256": digest("video"),
              "stimulus_sha256": digest("stimulus")}
    expected = {"schema_version": 1, "source": source,
                "required_fields": list(FIELDS), "excluded_fields": {}, "frames": []}
    observed = {"schema_version": 1, "source": copy.deepcopy(source), "frames": []}
    for sequence, frequency in enumerate(("24.150", "34.700")):
        identity = {"frame_id": f"frame-{sequence}", "image_sha256": digest(str(sequence)),
                    "source_frame_seq": sequence, "capture_ns": sequence * 100_000_000}
        values = dict(primary_band="K" if sequence == 0 else "Ka",
                      primary_frequency=frequency, primary_direction="front", count=2,
                      secondary=[dict(band="X", frequency="10.525", direction="rear"),
                                 dict(band="K", frequency="24.125", direction="side")],
                      main_volume=5, mute_volume=0, mode="A", muted=False)
        expected["frames"].append({**identity, "fields": {
            field: {"allowed": [copy.deepcopy(value)]} for field, value in values.items()}})
        observed["frames"].append({**identity, "inference_status": "complete", "anomalies": [],
                                   "fields": {field: {"state": "readable", "value": value}
                                              for field, value in values.items()}})
    return expected, observed


class VisualCompareTests(unittest.TestCase):
    def setUp(self) -> None:
        self.expected, self.observed = fixture()

    def check(self, verdict: str, *, matched: int, mismatched: int = 0,
              unresolved: int = 0, attempted: int = 18, excluded: int = 0) -> dict:
        result = compare(self.expected, self.observed)
        self.assertEqual(result["result"], verdict)
        self.assertEqual(result["counts"], {
            "required": matched + mismatched + unresolved, "attempted": attempted,
            "evaluated": matched + mismatched, "matched": matched,
            "mismatched": mismatched, "unresolved": unresolved, "excluded": excluded})
        json.dumps(result, allow_nan=False)
        return result

    def test_matching_samples_keep_identity_and_do_not_modify_inputs(self) -> None:
        original = copy.deepcopy((self.expected, self.observed))
        result = self.check("PASS", matched=18)
        self.assertEqual(result["kind"], "sampled_frame_comparison")
        self.assertEqual(result["source"], self.expected["source"])
        self.assertEqual(result["frames"][1]["capture_ns"], 100_000_000)
        self.assertEqual(result["frames"][0]["observed_identity"]["image_sha256"], digest("0"))
        self.assertEqual(len(result["input_content_sha256"]["observed"]), 64)
        self.assertEqual(len(result["comparator_sha256"]), 64)
        self.assertEqual((self.expected, self.observed), original)

    def test_narrow_scope_requires_explicit_exclusions(self) -> None:
        self.expected["required_fields"] = ["primary_band", "primary_frequency"]
        self.expected["excluded_fields"] = {field: "reader not qualified for this field"
                                             for field in FIELDS if field not in self.expected["required_fields"]}
        del self.observed["frames"][0]["fields"]["count"]
        self.check("PASS", matched=4, attempted=4, excluded=14)

    def test_incomplete_duplicate_empty_and_overlapping_scopes_are_inconclusive(self) -> None:
        for required, excluded in ((["primary_band"], {}), ([], dict.fromkeys(FIELDS, "excluded")),
                                   (list(FIELDS) + ["mode"], {}), (list(FIELDS), {"mode": "excluded"}),
                                   (list(FIELDS[:-1]), {"muted": ""}), ([["mode"]], {})):
            with self.subTest(required=required):
                self.expected["required_fields"], self.expected["excluded_fields"] = required, excluded
                self.check("INCONCLUSIVE", matched=0, unresolved=18)

    def test_wrong_content_is_a_traceable_discrepancy(self) -> None:
        self.observed["frames"][0]["fields"]["primary_direction"]["value"] = "rear"
        result = self.check("FAIL", matched=17, mismatched=1)
        check = next(item for item in result["frames"][0]["checks"] if item["result"] == "MISMATCH")
        self.assertEqual(check["expected"], {"allowed": ["front"]})
        self.assertEqual(check["observed"]["value"], "rear")

    def test_reordered_observations_are_bound_by_frame_identity(self) -> None:
        self.observed["frames"].reverse()
        self.check("PASS", matched=18)
        self.observed["frames"][0]["fields"], self.observed["frames"][1]["fields"] = (
            self.observed["frames"][1]["fields"], self.observed["frames"][0]["fields"])
        self.check("FAIL", matched=14, mismatched=4)

    def test_explicit_absence_is_distinct_from_missing_value(self) -> None:
        self.expected["frames"][0]["fields"]["primary_frequency"] = {"allowed": [None]}
        self.observed["frames"][0]["fields"]["primary_frequency"]["value"] = None
        self.check("PASS", matched=18)
        del self.observed["frames"][0]["fields"]["primary_frequency"]["value"]
        self.check("INCONCLUSIVE", matched=17, unresolved=1)

    def test_missing_required_fields_and_unresolved_rules_keep_denominators(self) -> None:
        del self.observed["frames"][0]["fields"]["count"]
        del self.expected["frames"][0]["fields"]["mode"]
        self.expected["frames"][1]["fields"]["muted"] = {"unresolved": "configuration not recorded"}
        self.check("INCONCLUSIVE", matched=15, unresolved=3, attempted=17)

    def test_unreadable_ambiguous_and_unknown_states_do_not_become_matches(self) -> None:
        for field, state in zip(FIELDS, ("unreadable", "ambiguous", "confident")):
            self.observed["frames"][0]["fields"][field]["state"] = state
        self.check("INCONCLUSIVE", matched=15, unresolved=3)

    def test_malformed_types_are_unresolved_and_frequency_text_remains_literal(self) -> None:
        self.observed["frames"][0]["fields"]["muted"]["value"] = 0
        self.observed["frames"][0]["fields"]["count"]["value"] = "2"
        self.observed["frames"][0]["fields"]["primary_frequency"]["value"] = "24.15"
        self.check("FAIL", matched=15, mismatched=1, unresolved=2)

    def test_malformed_expected_and_observed_values_cannot_match_or_claim_discrepancy(self) -> None:
        cases = (("count", {"bogus": 2}), ("count", "2"), ("main_volume", False),
                 ("mute_volume", 0.0), ("muted", 0), ("primary_band", {}),
                 ("primary_frequency", 24150), ("primary_direction", []), ("mode", 1),
                 ("secondary", None),
                 ("secondary", [dict(band="K", frequency="24.125", direction=1)]))
        for field, value in cases:
            for side in ("expected", "observed", "both", "alternative"):
                with self.subTest(field=field, side=side):
                    self.expected, self.observed = fixture()
                    allowed = self.expected["frames"][0]["fields"][field]["allowed"]
                    if side in ("expected", "both"):
                        allowed[:] = [value]
                    elif side == "alternative":
                        allowed.append(value)
                    if side in ("observed", "both"):
                        self.observed["frames"][0]["fields"][field]["value"] = value
                    self.check("INCONCLUSIVE", matched=17, unresolved=1)

    def test_null_scalar_fields_and_empty_secondary_list_are_valid_absence(self) -> None:
        for field in FIELDS:
            value = [] if field == "secondary" else None
            self.expected["frames"][0]["fields"][field] = {"allowed": [value]}
            self.observed["frames"][0]["fields"][field]["value"] = value
        self.check("PASS", matched=18)

    def test_secondary_order_is_ignored_but_duplicates_are_preserved(self) -> None:
        secondary = self.observed["frames"][0]["fields"]["secondary"]["value"]
        secondary.reverse()
        self.check("PASS", matched=18)
        secondary.append(copy.deepcopy(secondary[0]))
        self.check("FAIL", matched=17, mismatched=1)

    def test_malformed_secondary_is_unresolved(self) -> None:
        del self.observed["frames"][0]["fields"]["secondary"]["value"][0]["direction"]
        self.check("INCONCLUSIVE", matched=17, unresolved=1)

    def test_permitted_blink_states_require_an_explicit_rule(self) -> None:
        self.expected["frames"][0]["fields"]["primary_direction"]["allowed"].append(None)
        self.observed["frames"][0]["fields"]["primary_direction"]["value"] = None
        self.check("PASS", matched=18)

    def test_wrong_source_invalidates_apparent_discrepancies(self) -> None:
        self.observed["source"]["video_sha256"] = digest("other video")
        self.observed["frames"][0]["fields"]["count"]["value"] = 9
        self.check("INCONCLUSIVE", matched=0, unresolved=18)

    def test_stale_or_duplicate_frame_identity_cannot_pass(self) -> None:
        for key, value in (("image_sha256", digest("other pixels")), ("source_frame_seq", 99),
                           ("capture_ns", 999), ("source_frame_seq", False)):
            with self.subTest(key=key):
                self.expected, self.observed = fixture()
                self.observed["frames"][0][key] = value
                self.check("INCONCLUSIVE", matched=9, unresolved=9,
                           attempted=9 if value is False else 18)
        self.expected, self.observed = fixture()
        self.observed["frames"].append(copy.deepcopy(self.observed["frames"][0]))
        self.check("INCONCLUSIVE", matched=9, unresolved=9)

    def test_unexpected_frames_prevent_pass(self) -> None:
        extra = copy.deepcopy(self.observed["frames"][0])
        extra["frame_id"] = "unexpected"
        self.observed["frames"].append(extra)
        self.check("INCONCLUSIVE", matched=18)

    def test_failed_and_missing_inference_preserve_required_scope(self) -> None:
        self.observed["frames"][0]["inference_status"] = "failed"
        self.observed["frames"][1]["fields"] = {}
        self.check("INCONCLUSIVE", matched=0, unresolved=18, attempted=9)
        self.observed = None
        self.check("INCONCLUSIVE", matched=0, unresolved=18, attempted=0)

    def test_unresolved_anomalies_prevent_pass_but_do_not_erase_discrepancies(self) -> None:
        self.observed["frames"][0]["anomalies"] = ["unexplained visible content"]
        self.check("INCONCLUSIVE", matched=18)
        self.observed["frames"][1]["fields"]["count"]["value"] = 7
        self.check("FAIL", matched=17, mismatched=1)
        del self.observed["frames"][0]["anomalies"]
        self.check("FAIL", matched=17, mismatched=1)

    def test_explicit_anomaly_scope_limits_review_to_required_fields(self) -> None:
        self.expected["required_fields"] = ["count", "mode"]
        self.expected["excluded_fields"] = {field: "outside counter reader scope"
                                             for field in FIELDS if field not in ("count", "mode")}
        self.observed["anomaly_scope"] = ["mode", "count"]
        result = self.check("PASS", matched=4, attempted=4, excluded=14)
        self.assertEqual(result["scope"]["anomaly_scope"], ["mode", "count"])
        self.observed["frames"][0]["anomalies"] = ["uncertain glyph alignment"]
        result = self.check("INCONCLUSIVE", matched=4, attempted=4, excluded=14)
        self.assertIn("declared-field anomaly review is unresolved", result["errors"][0])
        del self.observed["frames"][0]["anomalies"]
        self.check("INCONCLUSIVE", matched=4, attempted=4, excluded=14)
        self.observed["frames"][1]["fields"]["count"]["value"] = 7
        self.check("FAIL", matched=3, mismatched=1, attempted=4, excluded=14)

    def test_invalid_anomaly_scope_cannot_pass_or_shrink_field_checks(self) -> None:
        self.expected["required_fields"] = ["count", "mode"]
        self.expected["excluded_fields"] = {field: "outside counter reader scope"
                                             for field in FIELDS if field not in ("count", "mode")}
        for scope in (None, [], "full_frame", {}, ["count"], ["count", "mode", "muted"],
                      ["count", "mode", "mode"], ["count", ["mode"]], ["count", "unknown"]):
            with self.subTest(scope=scope):
                self.observed["anomaly_scope"] = scope
                result = self.check("INCONCLUSIVE", matched=4, attempted=4, excluded=14)
                self.assertTrue(any("anomaly_scope" in error for error in result["errors"]))

    def test_absent_anomaly_scope_keeps_full_frame_review(self) -> None:
        result = self.check("PASS", matched=18)
        self.assertEqual(result["scope"]["anomaly_scope"], "full_frame")
        del self.observed["frames"][0]["anomalies"]
        result = self.check("INCONCLUSIVE", matched=18)
        self.assertIn("full-frame anomaly review is unresolved", result["errors"][0])

    def test_empty_scope_malformed_schema_and_non_json_input_cannot_pass(self) -> None:
        for observed in ({}, {**self.observed, "schema_version": True},
                         {**self.observed, "non_json": float("nan")}):
            with self.subTest(observed=observed):
                result = compare(self.expected, observed)
                self.assertEqual(result["result"], "INCONCLUSIVE")
                self.assertEqual(result["counts"]["unresolved"], 18)
                json.dumps(result, allow_nan=False)
        self.expected["frames"], self.observed["frames"] = [], []
        self.check("INCONCLUSIVE", matched=0, attempted=0)


class VisualCompareCliTests(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        self.expected, self.observed = fixture()
        self.expected_path = self.directory / "expected.json"
        self.observed_path = self.directory / "observed.json"
        self.output_path = self.directory / "result.json"
        self.write_inputs()

    def write_inputs(self) -> None:
        self.expected_path.write_text(json.dumps(self.expected), encoding="utf-8")
        self.observed_path.write_text(json.dumps(self.observed), encoding="utf-8")

    def invoke(self) -> subprocess.CompletedProcess:
        script = Path(__file__).resolve().parent / "bench" / "visual_compare.py"
        result = subprocess.run(
            [sys.executable, str(script), "--expected", str(self.expected_path),
             "--observed", str(self.observed_path), "--out", str(self.output_path)],
            capture_output=True, text=True, timeout=10)
        self.assertNotIn("Traceback", result.stderr)
        return result

    def record(self) -> dict:
        return json.loads(self.output_path.read_text(encoding="utf-8"))

    def test_matching_cli_result_explicitly_limits_qualification(self) -> None:
        result = self.invoke()
        self.assertEqual(result.returncode, 0)
        self.assertIn("Sampled record comparison: MATCH.", result.stdout)
        self.assertNotIn("PASS", result.stdout)
        self.assertIn("no reader, source, or full-run qualification", self.record()["qualification"])
        self.assertEqual(self.record()["counts"]["matched"], 18)

    def test_cli_discrepancy_returns_one(self) -> None:
        self.observed["frames"][0]["fields"]["count"]["value"] = 9
        self.write_inputs()
        self.assertEqual(self.invoke().returncode, 1)
        self.assertEqual(self.record()["counts"]["mismatched"], 1)

    def test_missing_input_produces_inconclusive_record(self) -> None:
        self.observed_path.unlink()
        self.assertEqual(self.invoke().returncode, 2)
        self.assertEqual(self.record()["result"], "INCONCLUSIVE")
        self.assertEqual(self.record()["counts"]["unresolved"], 18)
        self.assertIn("observed input could not be read as JSON", self.record()["errors"])

    def test_malformed_json_schema_and_encoding_produce_records(self) -> None:
        for index, raw in enumerate((b"{truncated", b'{"schema_version": true}', b"\xff")):
            with self.subTest(raw=raw):
                self.observed_path.write_bytes(raw)
                self.output_path = self.directory / f"result-{index}.json"
                self.assertEqual(self.invoke().returncode, 2)
                self.assertEqual(self.record()["result"], "INCONCLUSIVE")

    def test_existing_output_is_preserved(self) -> None:
        self.output_path.write_bytes(b"existing evidence\n")
        self.assertEqual(self.invoke().returncode, 2)
        self.assertEqual(self.output_path.read_bytes(), b"existing evidence\n")
        self.assertEqual(list(self.directory.glob(".visual-compare-*")), [])

    def test_duplicate_values_and_identities_are_inconclusive(self) -> None:
        raw = json.dumps(self.observed)
        identity = f'"image_sha256": "{digest("0")}"'
        variants = (raw.replace('"value": 2', '"value": 999, "value": 2', 1),
                    raw.replace(identity, f'"image_sha256": "{digest("different")}", {identity}', 1))
        for index, contradictory in enumerate(variants):
            with self.subTest(index=index):
                self.observed_path.write_text(contradictory, encoding="utf-8")
                self.output_path = self.directory / f"duplicate-{index}.json"
                self.assertEqual(self.invoke().returncode, 2)
                self.assertEqual(self.record()["result"], "INCONCLUSIVE")
                self.assertIn("observed input could not be read as JSON", self.record()["errors"])

    def test_cli_matching_malformed_fields_cannot_pass(self) -> None:
        self.expected["frames"][0]["fields"]["count"] = {"allowed": [{"bogus": 2}]}
        self.observed["frames"][0]["fields"]["count"]["value"] = {"bogus": 2}
        self.write_inputs()
        self.assertEqual(self.invoke().returncode, 2)
        self.assertEqual(self.record()["counts"]["unresolved"], 1)

    def test_failed_serialization_never_publishes_partial_output(self) -> None:
        with self.assertRaises(ValueError):
            _publish_new(self.output_path, {"partial": "prefix", "invalid": float("nan")})
        self.assertFalse(self.output_path.exists())
        self.assertEqual(list(self.directory.glob(".visual-compare-*")), [])


if __name__ == "__main__":
    unittest.main()
