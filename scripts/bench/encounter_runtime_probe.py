"""Operational self-test for the local encounter-card OCR helper."""

from __future__ import annotations

import base64
import hashlib
import json
from pathlib import Path
import re
import subprocess
from typing import Any


PROBE_FILE = "encounter_ocr_probe.b64"
EXPECTED_TEXT = "K24.150"


def probe_image_bytes() -> bytes:
    """Return the exact probe image, independent of its source-file encoding."""
    encoded = b"".join(Path(__file__).with_name(PROBE_FILE).read_bytes().split())
    return base64.b64decode(encoded, validate=True)


def probe_image_sha256() -> str:
    return hashlib.sha256(probe_image_bytes()).hexdigest()


def _normalized(text: Any) -> str:
    if not isinstance(text, str):
        return ""
    return re.sub(r"\s", "", text).translate(
        str.maketrans({"К": "K", "к": "K", "а": "a", "А": "A", "Х": "X", "х": "x"}))


def probe_ocr_runtime(setup: dict[str, Any]) -> dict[str, Any]:
    """Prove the compiled helper can recognize one representative fixed crop.

    Compilation alone is insufficient: macOS Vision may reject requests in a
    restricted execution environment.  The retained probe is display content,
    contains no expected run data, and is never used to classify a DUT frame.
    Probe the active helper: prepare_reader may reuse it across analyses that
    request different cache directories in the same process.
    """
    result: dict[str, Any] = {
        "status": "unavailable",
        "expected_text": EXPECTED_TEXT,
    }
    try:
        probe_bytes = probe_image_bytes()
        result["probe_sha256"] = hashlib.sha256(probe_bytes).hexdigest()
        if setup.get("ocr_available") is not True:
            raise RuntimeError("OCR helper did not compile")
        source_sha = setup.get("ocr_source_sha256")
        if not isinstance(source_sha, str) or not re.fullmatch(r"[0-9a-f]{64}", source_sha):
            raise RuntimeError("OCR source identity is unavailable")
        import encounter_reader
        binary = encounter_reader._ocr_binary
        if not isinstance(binary, Path) or not binary.is_file():
            raise RuntimeError("OCR helper binary is unavailable")
        binary_sha = hashlib.sha256(binary.read_bytes()).hexdigest()
        if binary_sha != setup.get("ocr_binary_sha256"):
            raise RuntimeError("OCR helper binary identity differs")
        encoded = base64.b64encode(probe_bytes).decode("ascii")
        completed = subprocess.run([str(binary)], input=json.dumps([encoded]) + "\n",
                                   text=True, capture_output=True, timeout=20)
        payload = json.loads(completed.stdout)
        crops = payload.get("crops")
        if completed.returncode or not isinstance(crops, list) or len(crops) != 1:
            raise RuntimeError("OCR helper rejected the representative crop")
        candidates = [candidate for row in crops[0].get("rows", [])
                      for candidate in row.get("candidates", [])
                      if isinstance(candidate, dict)]
        if not any(_normalized(candidate.get("text")) == EXPECTED_TEXT
                   and candidate.get("confidence", 0) >= .5 for candidate in candidates):
            raise RuntimeError("OCR helper did not read the representative crop")
        result.update(status="operational", binary_sha256=binary_sha)
    except (OSError, UnicodeError, ValueError, TypeError, KeyError,
            subprocess.SubprocessError, RuntimeError):
        result["reason"] = "representative local OCR request failed"
    return result
