#!/usr/bin/env python3
"""Tests for the operational OCR probe used by the product gate."""

import base64
import hashlib
import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch


sys.path.insert(0, str(Path(__file__).resolve().parent / "bench"))
import encounter_runtime_probe as runtime_probe
from encounter_runtime_probe import probe_ocr_runtime


class RuntimeProbeTests(unittest.TestCase):
    def test_missing_or_corrupt_probe_is_unavailable(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            setup = self.setup(root)
            with patch.object(runtime_probe, "__file__", str(root / "probe.py")):
                for content in (None, b"corrupt base64!"):
                    if content is not None:
                        (root / runtime_probe.PROBE_FILE).write_bytes(content)
                    result = probe_ocr_runtime(root, setup)
                    self.assertEqual(result["status"], "unavailable")

    def test_runtime_identifies_exact_image_instead_of_base64_wrapping(self):
        image = runtime_probe.probe_image_bytes()
        encoded = base64.b64encode(image)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            setup = self.setup(root)
            payload = {"crops": [{"rows": [{"candidates": [
                {"text": "K24.150", "confidence": .99}]}]}]}
            completed = SimpleNamespace(returncode=0, stdout=json.dumps(payload), stderr="")
            probe = root / runtime_probe.PROBE_FILE
            observations = []
            wrappers = (encoded, b"\n".join(encoded[i:i + 76] for i in range(0, len(encoded), 76)))
            self.assertNotEqual(hashlib.sha256(wrappers[0]).digest(),
                                hashlib.sha256(wrappers[1]).digest())
            with (patch.object(runtime_probe, "__file__", str(root / "probe.py")),
                  patch.object(runtime_probe.subprocess, "run", return_value=completed) as invoke):
                for wrapper in wrappers:
                    probe.write_bytes(wrapper)
                    observations.append(probe_ocr_runtime(root, setup))
                    sent = json.loads(invoke.call_args.kwargs["input"])[0]
                    self.assertEqual(base64.b64decode(sent, validate=True), image)
                self.assertEqual(observations[0], observations[1])
                self.assertEqual(observations[0]["probe_sha256"], hashlib.sha256(image).hexdigest())
                probe.write_bytes(base64.b64encode(image + b"changed"))
                self.assertNotEqual(runtime_probe.probe_image_sha256(),
                                    observations[0]["probe_sha256"])

    def setup(self, root):
        source_sha = "a" * 64
        binary = root / ("vision-" + source_sha[:16])
        binary.write_bytes(b"frozen helper")
        return {"ocr_available": True, "ocr_source_sha256": source_sha,
                "ocr_binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest()}

    def test_representative_read_is_operational(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            setup = self.setup(root)
            payload = {"crops": [{"rows": [{"candidates": [
                {"text": "K 24.150", "confidence": .99}]}]}]}
            completed = SimpleNamespace(returncode=0, stdout=json.dumps(payload), stderr="")
            with patch("encounter_runtime_probe.subprocess.run", return_value=completed):
                result = probe_ocr_runtime(root, setup)
            self.assertEqual(result["status"], "operational")
            self.assertEqual(result["binary_sha256"], setup["ocr_binary_sha256"])

    def test_compiled_helper_that_rejects_real_work_is_unavailable(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            setup = self.setup(root)
            completed = SimpleNamespace(returncode=0,
                                        stdout=json.dumps({"error": "local Vision OCR failed"}),
                                        stderr="")
            with patch("encounter_runtime_probe.subprocess.run", return_value=completed):
                result = probe_ocr_runtime(root, setup)
            self.assertEqual(result["status"], "unavailable")
            self.assertIn("representative", result["reason"])

    def test_binary_identity_drift_is_unavailable(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            setup = self.setup(root)
            setup["ocr_binary_sha256"] = "b" * 64
            result = probe_ocr_runtime(root, setup)
            self.assertEqual(result["status"], "unavailable")


if __name__ == "__main__":
    unittest.main()
