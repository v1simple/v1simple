"""Browser write-admission checks and optional actual-built-page integration.

Ordinary invocation needs only the standard library. To exercise Chrome against
the existing production web build, set BEHAVIOR_BROWSER_LIVE_TESTS=1 and use the
bench Python with requirements-bench-browser.txt installed. No DUT is contacted.
"""
from __future__ import annotations

from contextlib import contextmanager
from copy import deepcopy
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
import importlib.util
import json
import os
from pathlib import Path
import re
import socket
import tempfile
import threading
import unittest
from urllib.parse import urlencode, urlsplit, parse_qsl

from bench.behavior_browser import (
    BrowserDriver, COLOR_CONTROLS, DISPLAY_FIELDS, SaveWriteGuard, TOGGLES,
    canonical_display, form_values, normalize_base_url, target_display,
)

ROOT = Path(__file__).resolve().parents[1]
HEADERS = {"x-v1simple-request": "maintenance-ui",
           "content-type": "application/x-www-form-urlencoded;charset=UTF-8"}


def baseline():
    values = {key: index + 1 for index, key in enumerate(COLOR_CONTROLS)}
    values.update({key: False for key in TOGGLES})
    values["brightness"] = 200
    return values


class BrowserWriteTests(unittest.TestCase):
    def test_schema_matches_all_shipping_ui_submission_fields(self):
        source = (ROOT / "interface/src/lib/utils/colors.js").read_text()
        fields = re.search(r"export const COLOR_PARAM_ORDER = \[(.*?)\];", source, re.S)
        self.assertIsNotNone(fields)
        self.assertEqual(set(re.findall(r"'([^']+)'", fields[1])), DISPLAY_FIELDS)
        self.assertEqual(len(DISPLAY_FIELDS), 39)

    def test_missing_or_invalid_snapshot_field_refuses_admission(self):
        for key, value in (("freq", 0), ("freq", 65536), ("freq", True),
                           ("brightness", 256), ("brightness", 0),
                           ("freqUseBandColor", 1)):
            with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                canonical_display({**baseline(), key: value})
        value = baseline()
        del value["bar6"]
        with self.assertRaises(ValueError):
            canonical_display(value)

    def test_snapshot_accepts_read_only_compatibility_aliases(self):
        value = {**baseline(), "barS1": 123, "wifiIcon": 456}
        self.assertEqual(canonical_display(value), baseline())

    def test_patch_cannot_touch_other_settings(self):
        for patch in ({}, {"stealthEnabled": True}, {"freq": 0}, {"freq": False}):
            with self.subTest(patch=patch), self.assertRaises(ValueError):
                target_display(baseline(), patch)

    def test_target_keeps_every_unedited_field(self):
        original = baseline()
        before, target = target_display(original, {"freq": 0xF800, "bar3": 0x7E0})
        self.assertEqual(before, original)
        self.assertEqual(target, {**original, "freq": 0xF800, "bar3": 0x7E0})

    def guard(self):
        guard = SaveWriteGuard("http://127.0.0.1:18765", baseline())
        guard.armed = True
        return guard

    def test_only_one_correct_ui_post_is_forwarded(self):
        guard = self.guard()
        body = urlencode(form_values(baseline()))
        self.assertTrue(guard.admit("POST", guard.base_url + "/api/display/settings", HEADERS, body))
        self.assertFalse(guard.admit("POST", guard.base_url + "/api/display/settings", HEADERS, body))
        self.assertEqual(guard.forwarded, 1)
        self.assertEqual(guard.blocked, ["duplicate_write"])

    def test_unexpected_or_unarmed_writes_never_reach_device(self):
        body = urlencode(form_values(baseline()))
        for method, suffix, headers, request_body in (
            ("POST", "/api/quiet/settings", HEADERS, "stealthEnabled=true"),
            ("POST", "/api/display/settings?reset=1", HEADERS, body),
            ("PUT", "/api/display/settings", HEADERS, body),
            ("POST", "/api/display/settings", {}, body),
            ("POST", "/api/display/settings", HEADERS, body + "&freq=1"),
            ("POST", "/api/display/settings", HEADERS, body.replace("freq=2", "freq=999")),
            ("POST", "/api/display/settings", HEADERS, "bad"),
        ):
            with self.subTest(method=method, suffix=suffix):
                guard = self.guard()
                self.assertFalse(guard.admit(method, guard.base_url + suffix, headers, request_body))
                self.assertEqual(guard.forwarded, 0)
        guard = self.guard()
        guard.armed = False
        self.assertFalse(guard.admit("POST", guard.base_url + "/api/display/settings", HEADERS, body))

    def test_other_origins_are_blocked_even_for_reads(self):
        guard = self.guard()
        self.assertFalse(guard.admit("GET", "http://example.invalid/", {}, None))
        self.assertTrue(guard.admit("GET", guard.base_url + "/api/status", {}, None))

    def test_base_url_refuses_credentials_paths_and_invalid_ports(self):
        self.assertEqual(normalize_base_url("http://127.0.0.1:18765/"), "http://127.0.0.1:18765")
        for value in ("file:///tmp/test", "http://user:secret@localhost", "http://localhost/colors",
                      "http://localhost?token=secret", "http://localhost#x", "http://localhost:invalid"):
            with self.subTest(value=value), self.assertRaises(ValueError):
                normalize_base_url(value)


@contextmanager
def built_page_server(mode="normal"):
    """Serve unmodified built Svelte assets with modeled device HTTP/storage."""
    state = {"display": baseline(), "posts": [], "mode": mode}
    if mode == "baseline_drift":
        state["display"]["freq"] = 77

    class Handler(SimpleHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def send_json(self, value, status=200):
            body = json.dumps(value).encode()
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            path = urlsplit(self.path).path
            if path == "/api/display/settings":
                if mode == "initial_read_failure":
                    self.send_json({"error": "fixture"}, 503)
                else:
                    self.send_json(state["display"])
            elif path == "/api/quiet/settings":
                self.send_json({"stealthEnabled": False})
            elif path == "/api/status":
                self.send_json({"maintenanceBoot": True, "device": {"uptime": 10, "heap_free": 100000}})
            elif path == "/api/device/settings":
                self.send_json({"isDefaultPassword": False})
            elif path.startswith("/api/"):
                self.send_json({})
            else:
                if path == "/colors":
                    self.path = "/colors.html"
                super().do_GET()

        def do_POST(self):
            length = int(self.headers.get("Content-Length", "0"))
            raw = self.rfile.read(length).decode()
            state["posts"].append({"path": self.path, "body": raw,
                                   "ui_header": self.headers.get("X-V1Simple-Request")})
            if self.path != "/api/display/settings":
                self.send_json({"error": "unexpected"}, 400)
                return
            values = dict(parse_qsl(raw))
            decoded = {key: values[key] == "true" if key in TOGGLES else int(values[key])
                       for key in DISPLAY_FIELDS}
            if mode != "ignored_save":
                state["display"] = decoded
            if mode == "lost_response":
                self.connection.shutdown(socket.SHUT_RDWR)
                self.connection.close()
                return
            self.send_json({"success": True})

    server = ThreadingHTTPServer(("127.0.0.1", 0), partial(Handler, directory=ROOT / "interface/build"))
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield f"http://127.0.0.1:{server.server_port}", state
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)


@unittest.skipUnless(os.environ.get("BEHAVIOR_BROWSER_LIVE_TESTS") == "1",
                     "set BEHAVIOR_BROWSER_LIVE_TESTS=1 for the real browser / local fixture checks")
class BuiltPageBrowserTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not importlib.util.find_spec("playwright"):
            raise RuntimeError("Install requirements-bench-browser.txt for requested browser tests")
        if not (ROOT / "interface/build/colors.html").is_file():
            raise RuntimeError("Build the production web assets before requested browser tests")

    def exercise(self, mode, patch):
        with tempfile.TemporaryDirectory(prefix="behavior-browser-") as directory, built_page_server(mode) as (url, state):
            with BrowserDriver(url, directory, timeout_ms=20000,
                               channel=os.environ.get("BEHAVIOR_BROWSER_CHANNEL", "chrome")) as driver:
                result = driver.save_display(patch, baseline())
            records = list(Path(directory).glob("*.json"))
            self.assertEqual(len(records), 1)
            self.assertEqual(json.loads(records[0].read_text()), result)
            return result, deepcopy(state)

    def test_actual_page_edits_every_field_and_submits_once(self):
        patch = {key: (index + 100) for index, key in enumerate(COLOR_CONTROLS)}
        patch.update({key: True for key in TOGGLES})
        patch["brightness"] = 121
        result, state = self.exercise("normal", patch)
        self.assertEqual(result["result"], "PASS", result)
        self.assertEqual(state["display"], patch)
        self.assertEqual(result["readback"], patch)
        self.assertEqual(len(state["posts"]), 1)
        self.assertEqual(state["posts"][0]["ui_header"], "maintenance-ui")

    def test_failed_initial_read_does_not_send_a_save(self):
        result, state = self.exercise("initial_read_failure", {"freq": 0xF800})
        self.assertEqual(result["result"], "INCOMPLETE", result)
        self.assertEqual(result["reason"], "initial_display_read_failed")
        self.assertEqual(state["posts"], [])

    def test_changed_baseline_does_not_overwrite_the_device(self):
        result, state = self.exercise("baseline_drift", {"freq": 0xF800})
        self.assertEqual(result["result"], "INCOMPLETE", result)
        self.assertEqual(result["reason"], "baseline_changed_before_edit")
        self.assertEqual(state["posts"], [])

    def test_committed_save_with_lost_response_is_not_repeated(self):
        result, state = self.exercise("lost_response", {"freq": 0xF800})
        self.assertEqual(result["result"], "INCOMPLETE", result)
        self.assertEqual(result["mutation_outcome"], "possibly_committed")
        self.assertTrue(result["readback_matches"], result)
        self.assertEqual(state["display"]["freq"], 0xF800)
        self.assertEqual(len(state["posts"]), 1)

    def test_success_toast_with_ignored_write_is_a_failure(self):
        result, state = self.exercise("ignored_save", {"freq": 0xF800})
        self.assertEqual(result["result"], "FAIL", result)
        self.assertEqual(result["reason"], "successful_save_readback_mismatch")
        self.assertFalse(result["readback_matches"])
        self.assertEqual(len(state["posts"]), 1)


if __name__ == "__main__":
    unittest.main()
