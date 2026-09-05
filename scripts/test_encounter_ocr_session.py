#!/usr/bin/env python3
"""Exercise the owned OCR process and line protocol without requiring Vision."""
import json
from pathlib import Path
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent / "bench"))
from encounter_ocr_session import OCRSession


class OCRSessionTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.root = Path(self.directory.name)
        self.log = self.root / "calls.jsonl"

    def tearDown(self):
        self.directory.cleanup()

    def helper(self, body, *, before=""):
        path = self.root / "helper"
        path.write_text(f"#!{sys.executable}\n" + "import json, os, signal, sys, time\n" +
                        f"log = {str(self.log)!r}\n" + before + "\n" +
                        "for line in sys.stdin:\n" +
                        "    request = json.loads(line)\n" +
                        "    with open(log, 'a') as stream:\n" +
                        "        stream.write(json.dumps({'pid': os.getpid(), 'request': request}) + '\\n')\n" +
                        "    response = {'request_id': request['request_id'], 'crops': [\n" +
                        "        {'revision': 3, 'rows': [{'box': [0.1, 0.2, 0.8, 0.9],\n" +
                        "        'candidates': [{'text': crop, 'confidence': 0.75}]}]}\n" +
                        "        for crop in request['crops']]}\n" +
                        "\n".join("    " + part for part in body.splitlines()) + "\n")
        path.chmod(0o700)
        return path

    def calls(self):
        return [json.loads(line) for line in self.log.read_text().splitlines()]

    def test_reuses_one_helper_and_preserves_each_ordered_response(self):
        helper = self.helper("print(json.dumps(response), flush=True)")
        with OCRSession(helper) as session:
            first = session.request(["first crop", "second crop"])
            process = session._process
            second = session.request(["different frame"])
            self.assertEqual([c["rows"][0]["candidates"][0]["text"] for c in first],
                             ["first crop", "second crop"])
            self.assertEqual(second[0]["rows"][0]["candidates"][0]["text"], "different frame")
            self.assertIsNone(session.failure_reason)
        self.assertIsNotNone(process.poll())
        self.assertIsNone(session.request(["after close"]))
        calls = self.calls()
        self.assertEqual(len({call["pid"] for call in calls}), 1)
        self.assertEqual([call["request"]["request_id"] for call in calls], ["1", "2"])

    def test_reader_scope_reuses_encoded_crops_and_cleans_up_after_failure(self):
        import numpy as np
        import encounter_reader as reader
        helper = self.helper("print(json.dumps(response), flush=True)")
        crop = np.zeros((8, 12, 3), dtype=np.uint8)
        with (patch.object(reader, "prepare_reader"),
              patch.object(reader, "_ocr_binary", helper),
              patch.object(reader, "_ocr_session", None)):
            with self.assertRaisesRegex(RuntimeError, "stop analysis"):
                with reader.analysis_session():
                    first = reader._ocr([crop])
                    owned = reader._ocr_session
                    with reader.analysis_session():
                        second = reader._ocr([crop])
                        self.assertIs(reader._ocr_session, owned)
                    self.assertEqual(first, second)
                    process = owned._process
                    raise RuntimeError("stop analysis")
            self.assertIsNone(reader._ocr_session)
            self.assertIsNotNone(process.poll())
        calls = self.calls()
        self.assertEqual(len(calls), 2)
        self.assertEqual(calls[0]["pid"], calls[1]["pid"])
        self.assertEqual(calls[0]["request"]["crops"], calls[1]["request"]["crops"])

    def test_bad_reply_stops_helper_without_retry_or_next_frame(self):
        cases = [
            "response['request_id'] = 'wrong'\nprint(json.dumps(response), flush=True)",
            "response['crops'] = []\nprint(json.dumps(response), flush=True)",
            "response['crops'][0]['revision'] = 2\nprint(json.dumps(response), flush=True)",
            "response['crops'][0]['rows'][0]['box'][0] = True\nprint(json.dumps(response), flush=True)",
            "response['crops'][0]['rows'][0]['candidates'][0]['confidence'] = float('nan')\nprint(json.dumps(response), flush=True)",
            "print('{\"request_id\":\"wrong\",\"request_id\":\"1\",\"crops\":[]}', flush=True)",
            "print('{malformed', flush=True)",
            "print(json.dumps(response) + '\\n' + json.dumps(response), flush=True)",
            "print(json.dumps({'error': 'local Vision OCR failed'}), flush=True)",
            "sys.exit(2)",
        ]
        for body in cases:
            with self.subTest(body=body):
                if self.log.exists():
                    self.log.unlink()
                with OCRSession(self.helper(body)) as session:
                    self.assertIsNone(session.request(["one crop"]))
                    self.assertIsNotNone(session.failure_reason)
                    self.assertIsNone(session._process)
                    self.assertIsNone(session.request(["another frame"]))
                self.assertEqual(len(self.calls()), 1)

    def test_delayed_duplicate_cannot_supply_next_frames_result(self):
        body = ("if request['request_id'] == '2':\n"
                "    response['request_id'] = '1'\n"
                "print(json.dumps(response), flush=True)")
        with OCRSession(self.helper(body)) as session:
            self.assertIsNotNone(session.request(["first frame"]))
            self.assertIsNone(session.request(["second frame"]))
            self.assertIsNone(session.request(["third frame"]))
        self.assertEqual(len(self.calls()), 2)

    def test_native_boxes_outside_crop_are_preserved_without_clipping(self):
        box = [-0.000559092628981387, -0.0023859067765628073, 0.9259734630512673, 0.8496081308516109]
        helper = self.helper(f"response['crops'][0]['rows'][0]['box'] = {box!r}\n"
                             "print(json.dumps(response), flush=True)")
        with OCRSession(helper) as session:
            self.assertEqual(session.request(["crop"])[0]["rows"][0]["box"], box)

    def test_partial_response_and_blocked_input_both_have_deadlines(self):
        helpers = [("sys.stdout.write('{'); sys.stdout.flush(); time.sleep(60)", "", ["crop"]),
                   ("pass", "time.sleep(60)", ["x" * 1_000_000])]
        for body, before, crops in helpers:
            helper = self.helper(body, before=before)
            with self.subTest(crops=len(crops[0])):
                start = time.monotonic()
                with OCRSession(helper, timeout=.1) as session:
                    self.assertIsNone(session.request(crops))
                    self.assertIn("timed out", session.failure_reason)
                self.assertLess(time.monotonic() - start, 2)

    def test_oversized_unterminated_reply_is_bounded(self):
        helper = self.helper("sys.stdout.write('x' * 2_000_000); sys.stdout.flush(); time.sleep(60)")
        with OCRSession(helper) as session:
            self.assertIsNone(session.request(["crop"]))
            self.assertIn("byte limit", session.failure_reason)

    def test_context_exception_kills_helper_that_ignores_eof_and_terminate(self):
        helper = self.helper("print(json.dumps(response), flush=True)\ntime.sleep(60)",
                             before="signal.signal(signal.SIGTERM, signal.SIG_IGN)")
        start = time.monotonic()
        with self.assertRaisesRegex(RuntimeError, "analysis failed"):
            with OCRSession(helper) as session:
                self.assertIsNotNone(session.request(["crop"]))
                process = session._process
                raise RuntimeError("analysis failed")
        self.assertIsNotNone(process.poll())
        self.assertLess(time.monotonic() - start, 2)

    def test_missing_helper_and_oversized_request_do_not_start_replacements(self):
        with OCRSession(self.root / "missing") as session:
            self.assertIsNone(session.request(["crop"]))
            self.assertIsNone(session.request(["crop"]))
        with OCRSession(self.helper("print(json.dumps(response), flush=True)")) as session:
            self.assertIsNone(session.request(["x" * OCRSession.MAX_REQUEST_BYTES]))
            self.assertIn("byte limit", session.failure_reason)
            self.assertIsNone(session._process)
        self.assertFalse(self.log.exists())


if __name__ == "__main__":
    unittest.main()
