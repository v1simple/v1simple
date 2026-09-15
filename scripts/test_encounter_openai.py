#!/usr/bin/env python3
"""Privacy, consent and response-shape tests for the online frame observer."""
from __future__ import annotations

import base64
import json
import os
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent / "bench"))
import encounter_openai as observer


LITERAL = {
    "counter_glyph": "1",
    "primary_frequency": "34.700",
    "active_bands": ["Ka"],
    "main_arrows": ["front"],
    "main_bars": 1,
    "secondary": [],
    "muted_badge": False,
    "issues": [],
}


class Response:
    def __init__(self, body):
        self.body = body

    def __enter__(self):
        return self

    def __exit__(self, *_):
        return False

    def read(self, _limit):
        return self.body


def api_response(literal=LITERAL):
    return json.dumps({
        "status": "completed",
        "model": observer.MODEL,
        "output": [{"type": "message", "content": [
            {"type": "output_text", "text": json.dumps(literal)}
        ]}],
    }).encode()


class EncounterOpenAITests(unittest.TestCase):
    def setUp(self):
        observer._upload_authorized = False

    def test_request_contains_only_fixed_prompt_and_canonical_png(self):
        rgb = bytes((255, 0, 0, 0, 255, 0))
        png = observer.encode_png(rgb, 2, 1)
        request = observer.build_request(png)
        self.assertEqual(request["model"], observer.MODEL)
        self.assertIs(request["store"], False)
        self.assertEqual(request["tools"], [])
        self.assertEqual(request["input"][0]["content"][0],
                         {"type": "input_text", "text": observer.PROMPT})
        image = request["input"][0]["content"][1]
        self.assertEqual(image["detail"], "original")
        self.assertEqual(base64.b64decode(image["image_url"].split(",", 1)[1]), png)
        encoded = json.dumps(request)
        for forbidden in ("requestedHostMonotonicNs", "replayOffsetSeconds", "bytesHex",
                          "/Users/", ".artifacts/"):
            self.assertNotIn(forbidden, encoded)
        objects = []

        def visit(value):
            if isinstance(value, dict):
                objects.append(value)
                for child in value.values():
                    visit(child)
            elif isinstance(value, list):
                for child in value:
                    visit(child)

        visit(request)
        self.assertTrue(all("expected" not in value for value in objects))
        self.assertEqual([png[12:16], png[-8:-4]], [b"IHDR", b"IEND"])
        self.assertNotIn(b"tEXt", png)
        self.assertNotIn(b"eXIf", png)

    def test_prepare_requires_explicit_consent_and_api_key(self):
        with patch.dict(os.environ, {}, clear=True):
            with self.assertRaisesRegex(RuntimeError, "explicit"):
                observer.prepare_reader(allow_upload=False)
            with self.assertRaisesRegex(RuntimeError, "OPENAI_API_KEY"):
                observer.prepare_reader(allow_upload=True)
        with patch.dict(os.environ, {"OPENAI_API_KEY": "test-key-that-is-long-enough"}, clear=True):
            result = observer.prepare_reader(allow_upload=True)
        self.assertIs(result["response_storage_requested"], False)
        self.assertEqual(result["retry_count"], 0)

    def test_observe_sends_no_api_key_in_body_and_returns_literal_fields(self):
        secret = "test-key-that-must-not-enter-the-body"
        def open_request(request, timeout):
            self.assertEqual(timeout, 120)
            self.assertEqual(request.full_url, observer.API_URL)
            self.assertEqual(request.headers["Authorization"], f"Bearer {secret}")
            self.assertNotIn(secret.encode(), request.data)
            return Response(api_response())

        with patch.dict(os.environ, {"OPENAI_API_KEY": secret}, clear=True), \
             patch.object(observer.urllib.request, "urlopen", side_effect=open_request) as opened:
            observer.prepare_reader(allow_upload=True)
            width, height = observer.SUPPORTED_SIZE
            result = observer.observe(bytes(3 * width * height), width, height,
                                      {"private": "not sent"})
        self.assertEqual(opened.call_count, 1)
        self.assertEqual(result["fields"]["primary_frequency"]["value"], "34.700")
        self.assertEqual(result["fields"]["main_arrows"]["value"], ["front"])
        self.assertEqual(result["fields"]["main_bars"]["value"], 1)
        self.assertEqual(len(result["diagnostics"]["image_sha256"]), 64)

    def test_reported_issue_remains_unresolved_instead_of_becoming_absence(self):
        literal = dict(LITERAL)
        literal["main_arrows"] = []
        literal["issues"] = [{"field": "main_arrows", "reason": "ambiguous"}]
        reading = observer._reading(literal, observer.MODEL, "0" * 64)
        self.assertEqual(reading["fields"]["main_arrows"]["state"], "ambiguous")
        self.assertIsNone(reading["fields"]["main_arrows"]["value"])

    def test_literal_schema_represents_all_main_display_labels(self):
        labels = ("", "--.---", "LASER", "24.150", *observer.ALP_FREQUENCY_LABELS)
        for label in labels:
            with self.subTest(label=label):
                literal = dict(LITERAL, primary_frequency=label,
                               active_bands=["L", "Ka", "Ku", "K", "X"])
                reading = observer._reading(literal, observer.MODEL, "0" * 64)
                expected_state = "absent" if label == "" else "readable"
                self.assertEqual(reading["fields"]["primary_frequency"]["state"], expected_state)
                self.assertEqual(reading["fields"]["active_bands"]["value"],
                                 ["L", "Ka", "Ku", "K", "X"])

    def test_malformed_output_fails_without_retry(self):
        with patch.dict(os.environ, {"OPENAI_API_KEY": "test-key-that-is-long-enough"}, clear=True), \
             patch.object(observer.urllib.request, "urlopen",
                          return_value=Response(api_response({"main_bars": 1}))) as opened:
            observer.prepare_reader(allow_upload=True)
            with self.assertRaisesRegex(RuntimeError, "invalid"):
                width, height = observer.SUPPORTED_SIZE
                observer.observe(bytes(3 * width * height), width, height, {})
        self.assertEqual(opened.call_count, 1)

    def test_observe_rejects_unqualified_frame_size_before_network(self):
        with patch.dict(os.environ, {"OPENAI_API_KEY": "test-key-that-is-long-enough"}, clear=True):
            observer.prepare_reader(allow_upload=True)
        with patch.object(observer.urllib.request, "urlopen") as opened:
            with self.assertRaisesRegex(RuntimeError, "1280x720"):
                observer.observe(bytes(12), 2, 2, {})
        opened.assert_not_called()


if __name__ == "__main__":
    unittest.main()
