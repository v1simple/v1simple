"""Actual profile storage browser driver against built pages and controlled HTTP faults.

The server models storage and detector replies; these checks qualify the driver,
not a physical device. No production web asset is modified or intercepted.
"""
from contextlib import contextmanager
from copy import deepcopy
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
import importlib.util
import json
import os
from pathlib import Path
import socket
import sys
import tempfile
import threading
import unittest
from urllib.parse import parse_qs, urlsplit

sys.path.insert(0, str(Path(__file__).resolve().parent / "bench"))
from behavior_browser import BrowserDriver, ProfileWriteGuard, profile_storage_document

ROOT = Path(__file__).resolve().parents[1]
HEADERS = {"x-v1simple-request": "maintenance-ui", "content-type": "application/json"}


class ProfileWriteGuardTests(unittest.TestCase):
    def test_one_exact_json_write_and_duplicate_blocked(self):
        guard = ProfileWriteGuard("http://127.0.0.1:1234")
        guard.arm("/api/v1/profile/delete", {"name": "Temporary"})
        self.assertTrue(guard.admit("POST", guard.base_url + "/api/v1/profile/delete", HEADERS,
                                   '{"name":"Temporary"}'))
        self.assertFalse(guard.admit("POST", guard.base_url + "/api/v1/profile/delete", HEADERS,
                                    '{"name":"Temporary"}'))
        self.assertEqual(guard.forwarded, 1)

    def test_unarmed_wrong_origin_wrong_name_and_duplicate_member_never_forward(self):
        cases = [
            (False, "http://127.0.0.1:1234/api/v1/profile/delete", HEADERS, '{"name":"Temporary"}'),
            (True, "http://other.invalid/api/v1/profile/delete", HEADERS, '{"name":"Temporary"}'),
            (True, "http://127.0.0.1:1234/api/v1/profile/delete", HEADERS, '{"name":"Original"}'),
            (True, "http://127.0.0.1:1234/api/v1/profile/delete", HEADERS, '{"name":"Original","name":"Temporary"}'),
            (True, "http://127.0.0.1:1234/api/v1/profile/delete", {}, '{"name":"Temporary"}'),
            (True, "http://127.0.0.1:1234/api/v1/profile/delete?name=Original", HEADERS, '{"name":"Temporary"}'),
        ]
        for armed, url, headers, body in cases:
            with self.subTest(url=url, body=body, armed=armed):
                guard = ProfileWriteGuard("http://127.0.0.1:1234")
                if armed:
                    guard.arm("/api/v1/profile/delete", {"name": "Temporary"})
                self.assertFalse(guard.admit("POST", url, headers, body))
                self.assertEqual(guard.forwarded, 0)

    def test_json_boolean_is_not_numeric_alias(self):
        guard = ProfileWriteGuard("http://127.0.0.1:1234")
        guard.arm("/api/v1/profile", {"createOnly": True})
        self.assertFalse(guard.admit("POST", guard.base_url + "/api/v1/profile", HEADERS, '{"createOnly":1}'))

    def test_rearming_never_licenses_another_write(self):
        guard = ProfileWriteGuard("http://127.0.0.1:1234")
        guard.arm("/api/v1/profile/delete", {"name": "Temporary"})
        with self.assertRaises(ValueError):
            guard.arm("/api/v1/profile/delete", {"name": "Another"})


@contextmanager
def profile_server(mode="normal"):
    fixtures = json.loads((ROOT / "interface/src/test/fixtures/wifi-api.json").read_text())["scenarios"]
    capture = fixtures["v1_profile_routes"]["GET /api/v1/snapshot"][0]["body"]
    original = profile_storage_document("Original", "Preserve this profile")
    state = {"profiles": {"Original": original}, "posts": []}

    class Handler(SimpleHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def send_json(self, value, status=200):
            raw = json.dumps(value).encode()
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(raw)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(raw)

        def disconnect(self):
            self.connection.shutdown(socket.SHUT_RDWR)
            self.connection.close()

        def do_GET(self):
            parsed = urlsplit(self.path)
            if parsed.path == "/api/v1/profiles":
                if mode == "catalog_failure":
                    self.send_json({"error": "fixture read failure"}, 503)
                else:
                    rows = [{"name": name, "description": item["description"]}
                            for name, item in sorted(state["profiles"].items())]
                    self.send_json({"schemaVersion": 4, "detectorConfigurationOwner": "profile",
                                    "profiles": rows, "total": len(rows), "hasMore": False})
            elif parsed.path == "/api/v1/profile":
                name = parse_qs(parsed.query).get("name", [""])[0]
                if name not in state["profiles"]:
                    self.send_json({"error": "Profile not found"}, 404)
                else:
                    self.send_json(state["profiles"][name])
            elif parsed.path == "/api/v1/snapshot":
                self.send_json(capture)
            elif parsed.path == "/api/status":
                self.send_json({"maintenanceBoot": True, "maintenanceBootUptimeMs": 10000,
                                "device": {"hostname": "Fixture", "uptime": 10}})
            elif parsed.path == "/api/device/settings":
                self.send_json({"isDefaultPassword": False})
            elif parsed.path.startswith("/api/"):
                self.send_json({})
            else:
                if parsed.path == "/profiles":
                    self.path = "/profiles.html"
                super().do_GET()

        def do_POST(self):
            raw = self.rfile.read(int(self.headers.get("Content-Length", 0))).decode()
            state["posts"].append({"path": self.path, "body": raw,
                                   "ui_header": self.headers.get("X-V1Simple-Request")})
            if self.path == "/api/v1/profile":
                payload = json.loads(raw)
                name = payload["name"]
                if payload.get("createOnly") and name in state["profiles"]:
                    error = "Profile storage busy; retry" if mode == "collision_busy" else "Profile already exists"
                    # WifiApiResponse::setErrorAndMessage supplies both fields.
                    self.send_json({"error": error, "message": error}, 409)
                    return
                if mode != "ignored_edit" or payload.get("createOnly"):
                    payload.pop("createOnly", None)
                    payload["settings"].pop("baseBytes", None)
                    payload["settings"]["bytes"] = [255] * 6
                    state["profiles"][name] = payload
                if mode == "lost_create":
                    self.disconnect()
                else:
                    self.send_json({"success": True})
            elif self.path == "/api/v1/profile/delete":
                del state["profiles"][json.loads(raw)["name"]]
                self.send_json({"success": True})
            else:
                self.send_json({"error": "unexpected"}, 400)

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
                     "set BEHAVIOR_BROWSER_LIVE_TESTS=1 for built-page browser qualification")
class BuiltProfilesBrowserTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not importlib.util.find_spec("playwright"):
            raise RuntimeError("Install requirements-bench-browser.txt for requested browser tests")
        if not (ROOT / "interface/build/profiles.html").is_file():
            raise RuntimeError("Build production web assets before requested browser tests")

    @contextmanager
    def driver(self, mode="normal"):
        with tempfile.TemporaryDirectory(prefix="behavior-profiles-") as directory, profile_server(mode) as (url, state):
            with BrowserDriver(url, directory, timeout_ms=15000,
                               channel=os.environ.get("BEHAVIOR_BROWSER_CHANNEL", "chrome")) as driver:
                yield driver, state

    def test_complete_real_page_storage_roundtrip_preserves_original(self):
        with self.driver() as (driver, state):
            original = deepcopy(state["profiles"])
            new = profile_storage_document("Temporary", "Created")
            edited = {**new, "description": "Edited"}
            rejected = {**new, "description": "Must not replace"}
            stale = driver.prepare_profile_collision(rejected, ["Original"])
            created = driver.profile_create(new, ["Original"])
            self.assertEqual(created["result"], "PASS", created)
            changed = driver.profile_edit(new, edited)
            self.assertEqual(changed["result"], "PASS", changed)
            collision = driver.profile_collision(stale)
            self.assertEqual(collision["result"], "PASS", collision)
            self.assertEqual(collision["readback"], edited)
            driver.discard_profile_collision(stale)
            reloaded = driver.profile_reload(edited)
            self.assertEqual(reloaded["result"], "PASS", reloaded)
            deleted = driver.profile_delete(edited)
            self.assertEqual(deleted["result"], "PASS", deleted)
            self.assertEqual(state["profiles"], original)
            self.assertEqual(len(state["posts"]), 4)
            self.assertTrue(all(x["ui_header"] == "maintenance-ui" for x in state["posts"]))
            self.assertTrue(all(x["browser_closed"] for x in (created, changed, collision, reloaded, deleted)))

    def test_unrelated_conflict_does_not_prove_collision_preservation(self):
        with self.driver("collision_busy") as (driver, state):
            document = profile_storage_document("Temporary", "Created")
            stale = driver.prepare_profile_collision(document, ["Original"])
            created = driver.profile_create(document, ["Original"])
            self.assertEqual(created["result"], "PASS", created)
            before = deepcopy(state["profiles"])
            collision = driver.profile_collision(stale)
            self.assertEqual(collision["result"], "FAIL", collision)
            self.assertEqual(collision["reason"], "occupied_profile_not_rejected")
            self.assertEqual(collision["post_status"], 409)
            self.assertEqual(collision["post_count"], 1)
            self.assertTrue(collision["browser_closed"])
            self.assertEqual(state["profiles"], before)
            self.assertEqual(len(state["posts"]), 2)

    def test_failed_catalog_sends_nothing(self):
        with self.driver("catalog_failure") as (driver, state):
            result = driver.profile_create(profile_storage_document("Temporary", "Created"), ["Original"])
            self.assertEqual(result["result"], "INCOMPLETE", result)
            self.assertEqual(state["posts"], [])

    def test_lost_create_response_never_repeats_and_retains_actual_readback(self):
        with self.driver("lost_create") as (driver, state):
            document = profile_storage_document("Temporary", "Created")
            result = driver.profile_create(document, ["Original"])
            self.assertEqual(result["result"], "INCOMPLETE", result)
            self.assertEqual(result["mutation_outcome"], "possibly_committed")
            self.assertEqual(result["readback"], document)
            self.assertEqual(len(state["posts"]), 1)

    def test_success_message_does_not_hide_ignored_edit(self):
        with self.driver("ignored_edit") as (driver, state):
            original = deepcopy(state["profiles"]["Original"])
            result = driver.profile_edit(original, {**original, "description": "Ignored"})
            self.assertEqual(result["result"], "FAIL", result)
            self.assertEqual(state["profiles"]["Original"], original)
            self.assertEqual(len(state["posts"]), 1)

    def test_external_profile_change_prevents_delete(self):
        with self.driver() as (driver, state):
            expected = deepcopy(state["profiles"]["Original"])
            state["profiles"]["Original"]["description"] = "External edit"
            result = driver.profile_delete(expected)
            self.assertEqual(result["result"], "INCOMPLETE", result)
            self.assertEqual(state["posts"], [])

    def test_unsupported_create_or_edit_document_refused_before_write(self):
        with self.driver() as (driver, state):
            document = profile_storage_document("Temporary", "Created")
            document["settings"]["xBand"] = False
            with self.assertRaises(ValueError):
                driver.profile_create(document, ["Original"])
            before = deepcopy(state["profiles"]["Original"])
            after = deepcopy(before)
            after["settings"]["xBand"] = False
            with self.assertRaises(ValueError):
                driver.profile_edit(before, after)
            self.assertEqual(state["posts"], [])



if __name__ == "__main__":
    unittest.main()
