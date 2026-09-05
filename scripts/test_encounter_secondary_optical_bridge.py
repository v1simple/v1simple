#!/usr/bin/env python3
"""Focused checks for the bounded secondary text optical bridge."""
from __future__ import annotations

import base64
from copy import deepcopy
import hashlib
import json
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts" / "bench"))

import encounter_secondary_optical_bridge as bridge


FIELDS = ("counter_glyph", "primary_frequency", "active_bands", "main_arrows",
          "main_bars", "secondary", "muted_badge")
VALUE = [{"band": "K", "frequency": "24.150", "direction": "side", "bars": 3}]


def meter(count=3):
    states = ["on" if index < count else "off" for index in range(6)]
    return {"state": "readable", "value": count, "reason": None,
            "bars": [{"state": state} for state in states],
            "compatible_counts": [count]}


def exact_card():
    return {"slot": 0, **VALUE[0], "compatible_bars": [3], "text_visible": True,
            "bars_state": "readable", "bar_reading": meter(),
            "direction_reading": {"state": "readable", "value": "side", "reason": None,
                                  "shape_matches": ["side"]},
            "ocr_observation": {"revision": 3, "rows": [{"candidates": [
                {"text": "K 24.150", "confidence": 1.0}]}]},
            "ocr_candidates": [["K", "24.150"]]}


def refused_card():
    card = exact_card()
    card.update(band=None, frequency=None, ocr_candidates=[],
                ocr_observation={"revision": 3, "rows": [{"candidates": [
                    {"text": "24.150", "confidence": 1.0}]}]})
    return card


def profile(seed=96):
    raw = bytes((seed + index % 7 for index in range(bridge._PROFILE_BYTE_COUNT)))
    return {"schema_version": 1,
            "method_version": bridge.PROFILE_SECONDARY_PROBE_METHOD_VERSION,
            "profile_schema": deepcopy(bridge._PROFILE_SCHEMA),
            "cards": [{
                "slot": slot, "reference_bounds": list(box),
                "profile_b64": base64.b64encode(raw).decode("ascii"),
                "profile_sha256": hashlib.sha256(raw).hexdigest(),
            } for slot, box in enumerate(bridge._TEXT_BOXES)]}


def replace_profile(sample, *, changes):
    item = sample["observed"]["secondary_profiles"]["cards"][0]
    raw = bytearray(base64.b64decode(item["profile_b64"]))
    for index, delta in changes:
        raw[index] = max(0, min(255, raw[index] + delta))
    encoded = bytes(raw)
    item["profile_b64"] = base64.b64encode(encoded).decode("ascii")
    item["profile_sha256"] = hashlib.sha256(encoded).hexdigest()


def sample(index, *, refused=False):
    reading = ({"state": "unreadable", "value": None,
                "reason": bridge.RAW_UNREADABLE_REASON,
                "partial_cards": [{key: refused_card().get(key) for key in bridge._CARD_KEYS}],
                "cards": [refused_card()], "ocr_available": True}
               if refused else
               {"state": "readable", "value": deepcopy(VALUE), "reason": None,
                "cards": [exact_card()]})
    checks = {field: {"status": "MATCH"} for field in FIELDS}
    if refused:
        checks["secondary"] = {"status": "UNRESOLVED"}
    return {
        "frame_id": str(index), "video_frame_index": index,
        "source_frame_seq": 100 + index, "capture_ns": index * 5_000_000,
        "offset_seconds": index * .005, "image": f"frames/{index:06d}.png",
        "image_sha256": f"{index:064x}",
        "observed": {"fields": {"secondary": reading},
                     "secondary_profiles": profile()},
        "expected": {"fields": {"secondary": {"allowed": [deepcopy(VALUE)]}}},
        "comparison": {"checks": checks, "joint_state": {"status": "MATCH"}},
    }


def fixture():
    samples = [sample(1), sample(2), sample(3, refused=True), sample(4), sample(5)]
    event = {"event_id": "event-0001", "start_ns": 1, "end_ns": 30_000_000,
             "mode": "steady", "changed_fields": ["secondary"],
             "target": {"fields": {"secondary": {"allowed": [deepcopy(VALUE)]}}},
             "first_correct": {key: samples[0][key] for key in bridge._POINT_ID_KEYS}}
    context = {
        "capture_id": "1" * 64, "selection_manifest_sha256": "2" * 64,
        "verified_maximum_source_interval_ns": 5_000_000,
        "reader_method_version": bridge.PROFILE_READER_METHOD_VERSION,
        "reader_sha256": bridge.PROFILE_READER_SHA256,
        "secondary_probe_method_version": bridge.PROFILE_SECONDARY_PROBE_METHOD_VERSION,
        "secondary_probe_sha256": bridge.PROFILE_SECONDARY_PROBE_SHA256,
    }
    return samples, [event], context


class SecondaryOpticalBridgeTests(unittest.TestCase):
    def classify(self, samples, events, context):
        return bridge.classify_secondary_optical_bridge(samples, events, context)

    def test_exact_five_frame_bridge_preserves_raw_and_cannot_establish_acquisition(self):
        samples, events, context = fixture()
        frozen = deepcopy(samples)
        result = self.classify(samples, events, context)
        self.assertEqual(result["errors"], [])
        self.assertEqual(len(result["classifications"]), 1, result)
        record = result["classifications"][0]
        self.assertEqual(record["deadline_observation_semantics"],
                         "LEGAL_PRESENTATION_TRANSITION")
        self.assertEqual(record["video_frame_indices"], [3])
        self.assertEqual(record["resolved_value"], VALUE)
        self.assertEqual(record["current_presentation_established"]["video_frame_index"], 1)
        self.assertEqual(record["profile_metrics"]["target_envelope_violation_count"], 0)
        self.assertEqual(samples, frozen)

    def test_wrong_band_stroke_and_fade_pixels_are_rejected(self):
        cases = {
            "wrong_band": [(index, 70) for index in range(70)],
            "single_stroke": [(index, 45) for index in range(18)],
            "fade": [(index, -30) for index in range(bridge._PROFILE_BYTE_COUNT)],
        }
        for name, changes in cases.items():
            with self.subTest(name=name):
                samples, events, context = fixture()
                replace_profile(samples[2], changes=changes)
                result = self.classify(samples, events, context)
                self.assertEqual(result["classifications"], [])
                self.assertEqual(result["rejected_runs"][0]["code"], "OPTICAL_DIFFERENCE")

    def test_unstable_support_and_source_gap_are_rejected(self):
        samples, events, context = fixture()
        replace_profile(samples[0], changes=[(index, 40) for index in range(60)])
        result = self.classify(samples, events, context)
        self.assertEqual(result["classifications"], [])
        self.assertEqual(result["rejected_runs"][0]["code"], "OPTICAL_DIFFERENCE")

        samples, events, context = fixture()
        samples[2]["source_frame_seq"] += 4
        result = self.classify(samples, events, context)
        self.assertEqual(result["classifications"], [])
        self.assertEqual(result["rejected_runs"][0]["code"], "SOURCE_GAP")

    def test_malformed_base64_hash_and_profile_schema_are_rejected(self):
        corruptions = (
            lambda item: item.update(profile_b64="%%%"),
            lambda item: item.update(profile_sha256="0" * 64),
            lambda item: item.update(reference_bounds=[0, 0, 1, 1]),
        )
        for corrupt in corruptions:
            samples, events, context = fixture()
            corrupt(samples[2]["observed"]["secondary_profiles"]["cards"][0])
            result = self.classify(samples, events, context)
            self.assertEqual(result["classifications"], [])
            self.assertEqual(result["rejected_runs"][0]["code"], "OPTICAL_PROFILE")

    def test_unavailable_probe_rejects_without_mutating_raw_observation(self):
        samples, events, context = fixture()
        raw_secondary = deepcopy(samples[2]["observed"]["fields"]["secondary"])
        samples[2]["observed"]["secondary_profiles"] = {
            "schema_version": 1,
            "method_version": bridge.PROFILE_SECONDARY_PROBE_METHOD_VERSION,
            "status": "unavailable",
            "reason": "secondary probe failed: ValueError: registered region unavailable",
        }
        frozen = deepcopy(samples)
        result = self.classify(samples, events, context)
        self.assertEqual(result["classifications"], [])
        self.assertEqual(result["rejected_runs"][0]["code"], "OPTICAL_PROFILE")
        self.assertEqual(samples[2]["observed"]["fields"]["secondary"], raw_secondary)
        self.assertEqual(samples, frozen)

    def test_wrong_raw_token_sibling_or_missing_first_current_rejects(self):
        samples, events, context = fixture()
        samples[2]["observed"]["fields"]["secondary"]["cards"][0][
            "ocr_observation"]["rows"][0]["candidates"][0]["text"] = "35.500"
        result = self.classify(samples, events, context)
        self.assertEqual(result["classifications"], [])
        self.assertEqual(result["rejected_runs"][0]["code"], "CARD_EVIDENCE")

        samples, events, context = fixture()
        events[0].pop("first_correct")
        result = self.classify(samples, events, context)
        self.assertEqual(result["classifications"], [])
        self.assertEqual(result["rejected_runs"][0]["code"], "TARGET_ACQUISITION_CONTEXT")

    def test_spec_and_probe_hashes_are_exact(self):
        spec_path = ROOT / "scripts" / "bench" / "temporal_specs" / f"{bridge.CLASSIFIER_ID}.json"
        probe_path = ROOT / "scripts" / "bench" / "encounter_secondary_probe.py"
        self.assertEqual(hashlib.sha256(spec_path.read_bytes()).hexdigest(),
                         bridge.CLASSIFIER_SPEC_SHA256)
        self.assertEqual(hashlib.sha256(probe_path.read_bytes()).hexdigest(),
                         bridge.PROFILE_SECONDARY_PROBE_SHA256)
        spec = json.loads(spec_path.read_text())
        self.assertEqual(spec["validation"]["minimum_blind_true_admits"], 5)
        self.assertEqual(spec["validation"]["minimum_blind_true_rejects"], 5)
        self.assertEqual(spec["validation"]["required_false_admits"], 0)
        self.assertEqual(spec["validation"]["required_band_coverage"], ["X", "K", "Ka"])


if __name__ == "__main__":
    unittest.main()
