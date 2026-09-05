"""One local Vision process for one analysis; failed requests never retry.

The caller supplies exactly the same encoded PNGs as a one-shot invocation.
Request IDs bind each response to its input without interpreting its text.
"""
from __future__ import annotations

import json
import math
import os
import selectors
import subprocess
import time


def _object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate OCR response key")
        result[key] = value
    return result


def _number(value):
    return type(value) in (int, float) and math.isfinite(value)


def _validate(response, request_id, count):
    if (not isinstance(response, dict) or set(response) != {"request_id", "crops"}
            or response["request_id"] != request_id
            or not isinstance(response["crops"], list) or len(response["crops"]) != count):
        raise ValueError("OCR response does not match request")
    for crop in response["crops"]:
        if (not isinstance(crop, dict) or set(crop) != {"revision", "rows"}
                or type(crop["revision"]) is not int or crop["revision"] != 3
                or not isinstance(crop["rows"], list)):
            raise ValueError("invalid OCR crop response")
        for row in crop["rows"]:
            if not isinstance(row, dict) or set(row) != {"box", "candidates"}:
                raise ValueError("invalid OCR row response")
            box, candidates = row["box"], row["candidates"]
            # Vision can report boxes extending slightly outside the crop.
            # Preserve those coordinates; this validates transport, not ink.
            if (not isinstance(box, list) or len(box) != 4 or not all(map(_number, box))
                    or box[0] > box[2] or box[1] > box[3]
                    or not isinstance(candidates, list) or not 1 <= len(candidates) <= 3):
                raise ValueError("invalid OCR row geometry or candidates")
            for candidate in candidates:
                if (not isinstance(candidate, dict) or set(candidate) != {"text", "confidence"}
                        or not isinstance(candidate["text"], str)
                        or not _number(candidate["confidence"])
                        or not 0 <= candidate["confidence"] <= 1):
                    raise ValueError("invalid OCR candidate")
    return response["crops"]


class OCRSession:
    """Sequential, bounded line protocol. Use as an analysis-scoped context."""

    MAX_REQUEST_BYTES = 4 * 1024 * 1024
    MAX_RESPONSE_BYTES = 1024 * 1024

    def __init__(self, binary, *, timeout=20):
        if not math.isfinite(timeout) or timeout <= 0:
            raise ValueError("OCR timeout must be positive and finite")
        self.binary = str(binary)
        self.timeout = timeout
        self.failure_reason = None
        self._process = None
        self._closed = False
        self._request_id = 0

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

    def request(self, encoded):
        """Return unmodified crop observations, or None for terminal failure."""
        if self._closed:
            return None
        try:
            if not isinstance(encoded, list) or not all(isinstance(item, str) for item in encoded):
                raise ValueError("OCR request requires encoded PNG strings")
            self._request_id += 1
            request_id = str(self._request_id)
            payload = (json.dumps({"request_id": request_id, "crops": encoded}) + "\n").encode()
            if len(payload) > self.MAX_REQUEST_BYTES:
                raise ValueError("OCR request exceeds byte limit")
            deadline = time.monotonic() + self.timeout
            if self._process is None:
                self._process = subprocess.Popen([self.binary], stdin=subprocess.PIPE,
                                                 stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                                 bufsize=0)
                os.set_blocking(self._process.stdin.fileno(), False)
                os.set_blocking(self._process.stdout.fileno(), False)
            response = self._exchange(payload, deadline)
            return _validate(json.loads(response, object_pairs_hook=_object), request_id, len(encoded))
        except (OSError, subprocess.SubprocessError, ValueError, TimeoutError) as error:
            self.failure_reason = str(error)
            self.close()
            return None

    def _exchange(self, payload, deadline):
        process = self._process
        if process.poll() is not None:
            raise ValueError("OCR helper exited")
        received, sent = bytearray(), 0
        with selectors.DefaultSelector() as selector:
            selector.register(process.stdin, selectors.EVENT_WRITE)
            selector.register(process.stdout, selectors.EVENT_READ)
            while True:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise TimeoutError("OCR request timed out")
                for key, _ in selector.select(remaining):
                    if key.fileobj is process.stdin:
                        sent += os.write(process.stdin.fileno(), payload[sent:])
                        if sent == len(payload):
                            selector.unregister(process.stdin)
                    else:
                        chunk = os.read(process.stdout.fileno(), 65536)
                        if not chunk:
                            raise ValueError("OCR helper closed its response stream")
                        received.extend(chunk)
                        if len(received) > self.MAX_RESPONSE_BYTES:
                            raise ValueError("OCR response exceeds byte limit")
                        if b"\n" in received:
                            line, extra = received.split(b"\n", 1)
                            if extra or sent != len(payload):
                                raise ValueError("unsolicited OCR response data")
                            if process.poll() is not None:
                                raise ValueError("OCR helper exited after response")
                            return line

    def close(self):
        """Close input, then terminate a stuck owned helper within bounded waits."""
        self._closed = True
        process = self._process
        if process is None:
            return
        process.stdin.close()
        try:
            process.wait(timeout=.25)
        except subprocess.TimeoutExpired:
            process.terminate()
            try:
                process.wait(timeout=.25)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=1)
        finally:
            process.stdout.close()
            self._process = None
