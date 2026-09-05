#!/usr/bin/env python3
"""Tests for the operational OCR probe used by the product gate."""

import hashlib
import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch


sys.path.insert(0, str(Path(__file__).resolve().parent / "bench"))
from encounter_runtime_probe import probe_ocr_runtime


class RuntimeProbeTests(unittest.TestCase):
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
