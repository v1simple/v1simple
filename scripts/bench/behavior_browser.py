"""Guarded Display and profile operations through the shipping browser pages.

The caller owns the recovery journal, USB identity, maintenance admission and
independent device readback. This driver never retries a write, restores data,
sends USB mode commands, or starts a preview. Playwright is imported on entry.
"""
from __future__ import annotations

import json
import re
import subprocess
import time
from copy import deepcopy
from pathlib import Path
from urllib.parse import parse_qsl, urlencode, urlsplit


COLOR_CONTROLS = {
    "bogey": "bogey-color", "freq": "freq-color",
    "arrowFront": "arrow-front-color", "arrowSide": "arrow-side-color",
    "arrowRear": "arrow-rear-color", "bandL": "bandL-color",
    "bandKa": "bandKa-color", "bandK": "bandK-color", "bandX": "bandX-color",
    "bandPhoto": "band-photo-color", "wifiConnected": "wifiConnected-color",
    "bleConnected": "bleConnected-color", "bleDisconnected": "bleDisconnected-color",
    **{f"bar{i}": f"bar-{i}-color" for i in range(1, 7)},
    "muted": "muted-color", "persisted": "persisted-color",
    "volumeMain": "volumeMain-color", "volumeMute": "volumeMute-color",
    "rssiV1": "rssiV1-color", "rssiProxy": "rssiProxy-color", "obd": "obd-color",
    "alpConnected": "alp-connected-color", "alpDli": "alp-dli-color",
    "alpLidActive": "alp-lid-active-color", "alpAlert": "alp-alert-color",
}
TOGGLES = {
    "freqUseBandColor": ("Use band color for frequency", False),
    "hideWifiIcon": ("Show WiFi status", True),
    "hideProfileIndicator": ("Show active slot", True),
    "hideBatteryIcon": ("Show battery level", True),
    "showBatteryPercent": ("Show battery percentage", False),
    "hideBleIcon": ("Show Bluetooth proxy status", True),
    "hideVolumeIndicator": ("Show V1 volume", True),
    "hideRssiIndicator": ("Show connection strength", True),
}
DISPLAY_FIELDS = frozenset(COLOR_CONTROLS) | frozenset(TOGGLES) | {"brightness"}
DISPLAY_PATH = "/api/display/settings"
SUCCESS_MESSAGE = "Colors saved! Previewing on display..."
TERMINAL_MESSAGES = (
    SUCCESS_MESSAGE, "Failed to save colors", "Connection error",
    "Failed to load colors", "Failed to load quiet screen setting. Retry before changing it.",
)
ROOT = Path(__file__).resolve().parents[2]


def _same_json(left, right):
    return json.dumps(left, sort_keys=True, allow_nan=False) == json.dumps(right, sort_keys=True, allow_nan=False)


def _unique_json(pairs):
    value = {}
    for key, item in pairs:
        if key in value:
            raise ValueError("Duplicate JSON member")
        value[key] = item
    return value


def _profile_document(value):
    from usb_profiles import canonical_name, validate_detector, validate_in_the_box
    if (not isinstance(value, dict) or set(value) != {
            "schemaVersion", "name", "description", "settings", "detector", "inTheBox"}
            or type(value["schemaVersion"]) is not int or value["schemaVersion"] != 4
            or not canonical_name(value["name"]) or not isinstance(value["description"], str)
            or len(value["description"].encode()) > 4096 or not isinstance(value["settings"], dict)):
        raise ValueError("Complete version4 profile document required")
    raw = value["settings"].get("bytes")
    if (not isinstance(raw, list) or len(raw) != 6
            or any(type(item) is not int or not 0 <= item <= 255 for item in raw)):
        raise ValueError("Complete profile user bytes required")
    validate_detector(value["detector"], 4)
    validate_in_the_box(value["inTheBox"])
    return deepcopy(value)


def profile_storage_document(name: str, description: str) -> dict:
    """The shipping New defaults, with an independent factory-byte expectation."""
    if not isinstance(description, str) or description != description.strip():
        raise ValueError("Temporary profile description must already be trimmed")
    script = """
import {createDefaultProfileSettings,createProfileDetectorConfiguration,toApiSettings,toApiDetectorConfiguration}
 from './interface/src/lib/features/profiles/profileSettingsAdapter.js';
import {createDefaultInTheBoxSettings,toApiInTheBoxSettings}
 from './interface/src/lib/features/profiles/inTheBoxSettings.js';
console.log(JSON.stringify({settings:toApiSettings(createDefaultProfileSettings()),
 detector:toApiDetectorConfiguration(createProfileDetectorConfiguration()),
 inTheBox:toApiInTheBoxSettings(createDefaultInTheBoxSettings())}));
"""
    defaults = json.loads(subprocess.check_output(
        ["node", "--input-type=module", "-e", script], cwd=ROOT, text=True, timeout=15))
    defaults["settings"]["bytes"] = [255] * 6
    return _profile_document({"schemaVersion": 4, "name": name, "description": description, **defaults})


def _profile_payload(document: dict, *, creating: bool) -> dict:
    payload = _profile_document(document)
    raw = payload["settings"].pop("bytes")
    if creating:
        payload["createOnly"] = True
    else:
        # Loading a saved profile supplies fromApiSettings.baseBytes so an
        # intentional edit preserves bits which this UI does not expose.
        payload["settings"]["baseBytes"] = raw
    return payload


class ProfileWriteGuard:
    """Block all writes except one armed, exact profile request."""

    def __init__(self, base_url):
        self.base_url = normalize_base_url(base_url)
        self.expected = None
        self.forwarded = 0
        self.blocked = []

    def arm(self, path, payload):
        if self.expected is not None or self.forwarded:
            raise ValueError("Profile request already armed")
        if path not in ("/api/v1/profile", "/api/v1/profile/delete"):
            raise ValueError("Unsupported profile request")
        self.expected = (path, deepcopy(payload))

    def admit(self, method, url, headers, body):
        parsed = urlsplit(url)
        reason = None
        if f"{parsed.scheme}://{parsed.netloc}" != self.base_url:
            reason = "unexpected_origin"
        elif method in ("GET", "HEAD"):
            return True
        elif method != "POST" or parsed.query or parsed.fragment:
            reason = "unexpected_write"
        elif self.expected is None:
            reason = "unarmed_write"
        elif self.forwarded:
            reason = "duplicate_write"
        elif parsed.path != self.expected[0] or headers.get("x-v1simple-request") != "maintenance-ui":
            reason = "unexpected_write_contract"
        else:
            try:
                content = headers.get("content-type", "").lower()
                if not content.startswith("application/json"):
                    raise ValueError("encoding")
                value = json.loads(body or "", object_pairs_hook=_unique_json,
                                   parse_constant=lambda _: (_ for _ in ()).throw(ValueError("constant")))
                if not _same_json(value, self.expected[1]):
                    raise ValueError("payload")
            except (ValueError, TypeError):
                reason = "unexpected_write_payload"
        if reason:
            self.blocked.append(reason)
            return False
        self.forwarded = 1
        return True


class _ProfilePage:
    """One real page and one write; retained only for the stale-catalog witness."""

    def __init__(self, driver, label):
        if driver._browser is None:
            raise RuntimeError("BrowserDriver must be entered")
        self.driver, self.label = driver, label
        self.guard = ProfileWriteGuard(driver.base_url)
        self.context = driver._browser.new_context(service_workers="block", viewport={"width": 1280, "height": 1000})
        self.page = self.context.new_page()
        self.page.set_default_timeout(driver.timeout_ms)
        self.responses, self.errors, self.pending_responses = [], [], []
        self.finished_requests, self.failed_requests = set(), set()
        self.closed = False
        self.context.route("**/*", self._route)
        self.page.on("response", self._response)
        self.page.on("pageerror", lambda error: self.errors.append(type(error).__name__))
        self.page.on("requestfailed", self._request_failed)
        self.page.on("requestfinished", lambda request: self.finished_requests.add(request))

    def _route(self, route):
        request = route.request
        if self.guard.admit(request.method, request.url, request.headers, request.post_data):
            route.continue_()
        else:
            route.abort("blockedbyclient")

    def _response(self, response):
        path = urlsplit(response.url).path
        if path not in ("/api/status", "/api/v1/profiles", "/api/v1/profile", "/api/v1/profile/delete"):
            return
        item = {"path": path, "method": response.request.method, "status": response.status}
        self.responses.append(item)
        # A successful save need not consume its response body. Record its
        # authoritative status without waiting for an unused browser body.
        if response.request.method == "POST" and path in ("/api/v1/profile", "/api/v1/profile/delete") and response.status == 200:
            return
        # Never make nested synchronous Playwright calls from event callbacks,
        # or read a body before the browser has completed that request.
        self.pending_responses.append((response, item))

    def drain_responses(self):
        pending, self.pending_responses = self.pending_responses, []
        for response, item in pending:
            if response.request in self.failed_requests:
                item["body_unreadable"] = True
                continue
            if response.request not in self.finished_requests:
                self.pending_responses.append((response, item))
                continue
            try:
                item["body"] = response.json()
            except Exception:
                item["body_unreadable"] = True

    def response_json(self, response):
        deadline = time.monotonic() + self.driver.timeout_ms / 1000
        while response.request not in self.finished_requests:
            if response.request in self.failed_requests or time.monotonic() >= deadline:
                raise TimeoutError("Profile response body did not complete")
            self.page.wait_for_timeout(50)
        self.drain_responses()
        return response.json()

    def _request_failed(self, request):
        self.failed_requests.add(request)

    def open(self, catalog_names=None):
        from playwright.sync_api import expect
        self.page.goto(self.driver.base_url + "/profiles", wait_until="domcontentloaded")
        expect(self.page.get_by_role("button", name="New Profile", exact=True).first).to_be_enabled()
        deadline = time.monotonic() + self.driver.timeout_ms / 1000
        while time.monotonic() < deadline:
            self.drain_responses()
            catalogs = [x for x in self.responses if x["path"] == "/api/v1/profiles" and x["method"] == "GET"]
            maintenance = [x for x in self.responses if x["path"] == "/api/status" and x.get("body", {}).get("maintenanceBoot") is True]
            if catalogs and catalogs[-1].get("body", {}).get("hasMore") is False and maintenance:
                if any(x["status"] != 200 or x.get("body", {}).get("schemaVersion") != 4 for x in catalogs):
                    raise ValueError("Profile catalog read failed")
                rows = [row for x in catalogs for row in x["body"]["profiles"]]
                names = [row["name"] for row in rows]
                if (len(names) != len(set(names)) or catalogs[-1]["body"]["total"] != len(names)
                        or (catalog_names is not None and set(names) != set(catalog_names))):
                    raise ValueError("Complete profile catalog differs")
                self.catalog_names = names
                self.require_clean()
                return
            self.page.wait_for_timeout(50)
        raise TimeoutError("Complete profile catalog or maintenance observation missing")

    def require_clean(self):
        if self.guard.blocked or self.errors:
            raise ValueError("Browser profile contract failed")

    def row(self, name):
        from playwright.sync_api import expect
        summary = self.page.get_by_text(re.compile(r"^Browse saved profiles \(")).first
        if summary.is_visible() and summary.locator("xpath=ancestor::details").get_attribute("open") is None:
            summary.click()
        row = self.page.locator(".surface-panel").filter(has=self.page.locator(".font-medium").filter(
            has_text=re.compile("^" + re.escape(name) + "$")))
        expect(row).to_have_count(1)
        return row

    def edit(self, document):
        from playwright.sync_api import expect
        self.row(document["name"]).get_by_role("button", name="Edit", exact=True).click()
        editor = self.page.locator("#profile-editor")
        expect(editor.get_by_text("Editing profile: " + document["name"], exact=True)).to_be_visible()
        self.drain_responses()
        if not any(x["path"] == "/api/v1/profile" and x["status"] == 200 and
                   _same_json(x.get("body"), document) for x in self.responses):
            raise ValueError("Loaded profile differs from complete expected document")
        expect(editor.get_by_label("Description", exact=True)).to_have_value(document["description"])
        self.require_clean()
        return editor

    def draft(self, document):
        from playwright.sync_api import expect
        if any(name.lower() == document["name"].lower() for name in self.catalog_names):
            raise ValueError("Temporary profile already exists")
        self.page.get_by_role("button", name="New Profile", exact=True).first.click()
        self.page.locator("#profile-editor").get_by_label("Description", exact=True).fill(document["description"])
        self.page.get_by_role("button", name="Save as Profile", exact=True).click()
        dialog = self.page.get_by_role("dialog", name="Save Profile")
        dialog.get_by_label("Profile Name", exact=True).fill(document["name"])
        expect(dialog.locator("#profile-description")).to_have_value(document["description"])
        return dialog

    def read_profile(self, name, *, absent=False):
        deadline = time.monotonic() + self.driver.timeout_ms / 1000
        while time.monotonic() < deadline:
            reply = self.context.request.get(self.driver.base_url + "/api/v1/profile?" + urlencode({"name": name}),
                                             timeout=max(1, min(5000, int((deadline-time.monotonic())*1000))),
                                             max_redirects=0)
            try:
                if reply.status == 404 and absent:
                    return None
                value = reply.json()
                if reply.status == 409 and value == {"error": "Profile storage busy; retry"}:
                    self.page.wait_for_timeout(100)
                    continue
                if reply.status != 200 or absent:
                    raise ValueError("Profile readback status differs")
                return _profile_document(value)
            finally:
                reply.dispose()
        raise TimeoutError("Profile readback deadline")

    def send(self, button, path, payload):
        from behavior_state import atomic_json
        self.require_clean()
        atomic_json(self.driver.artifact_dir / f"{self.label}-intent.json",
                    {"path": path, "payload": payload, "outcome": "UNKNOWN"})
        self.guard.arm(path, payload)
        with self.page.expect_response(lambda r: r.request.method == "POST" and
                                       r.url == self.driver.base_url + path) as pending:
            button.click()
        response = pending.value
        self.drain_responses()
        self.require_clean()
        if self.guard.forwarded != 1:
            raise ValueError("Expected exactly one forwarded profile request")
        return response

    def close(self):
        if not self.closed:
            self.context.close()
            self.closed = True


def normalize_base_url(base_url: str) -> str:
    parsed = urlsplit(base_url)
    if (parsed.scheme not in ("http", "https") or not parsed.hostname
            or parsed.username is not None or parsed.password is not None
            or parsed.path not in ("", "/") or parsed.query or parsed.fragment):
        raise ValueError("Browser base URL must be an HTTP origin without credentials or path")
    # Force invalid port syntax to fail before the browser or any write exists.
    _ = parsed.port
    return f"{parsed.scheme}://{parsed.netloc}".rstrip("/")


def canonical_display(value: object) -> dict:
    """Require every current UI field; ignore API compatibility aliases only."""
    if not isinstance(value, dict) or not DISPLAY_FIELDS <= value.keys():
        raise ValueError("Display snapshot is incomplete")
    result = {}
    for key in sorted(DISPLAY_FIELDS):
        item = value[key]
        if key in TOGGLES:
            valid = type(item) is bool
        else:
            maximum = 255 if key == "brightness" else 65535
            valid = type(item) is int and 1 <= item <= maximum
        if not valid:
            raise ValueError(f"Display snapshot has an invalid {key} value")
        result[key] = item
    return result


def target_display(expected_before: dict, patch: dict) -> tuple[dict, dict]:
    baseline = canonical_display(expected_before)
    if not isinstance(patch, dict) or not patch or not patch.keys() <= DISPLAY_FIELDS:
        raise ValueError("Display patch must contain supported fields")
    return baseline, canonical_display({**baseline, **patch})


def form_values(value: dict) -> dict[str, str]:
    return {key: str(item).lower() if type(item) is bool else str(item)
            for key, item in canonical_display(value).items()}


class SaveWriteGuard:
    """Admit exactly one unchanged UI request matching the prepared target."""

    def __init__(self, base_url: str, target: dict):
        self.base_url = normalize_base_url(base_url)
        self.target = form_values(target)
        self.armed = False
        self.forwarded = 0
        self.blocked = []
        self.sent_monotonic_ns = None

    def admit(self, method: str, url: str, headers: dict, body: str | None) -> bool:
        parsed = urlsplit(url)
        if f"{parsed.scheme}://{parsed.netloc}" != self.base_url:
            self.blocked.append("unexpected_origin")
            return False
        if method in ("GET", "HEAD"):
            return True
        reason = None
        if method != "POST" or parsed.path != DISPLAY_PATH or parsed.query or parsed.fragment:
            reason = "unexpected_write"
        elif not self.armed:
            reason = "write_before_save"
        elif self.forwarded:
            reason = "duplicate_write"
        elif headers.get("x-v1simple-request") != "maintenance-ui":
            reason = "missing_ui_write_header"
        elif not headers.get("content-type", "").lower().startswith("application/x-www-form-urlencoded"):
            reason = "unexpected_write_encoding"
        else:
            try:
                pairs = parse_qsl(body or "", keep_blank_values=True, strict_parsing=True,
                                  max_num_fields=len(DISPLAY_FIELDS) + 1)
                if len(pairs) != len(DISPLAY_FIELDS) or len(dict(pairs)) != len(pairs):
                    reason = "unexpected_write_fields"
                elif dict(pairs) != self.target:
                    reason = "unexpected_write_values"
            except ValueError:
                reason = "malformed_write"
        if reason:
            self.blocked.append(reason)
            return False
        self.forwarded = 1
        self.sent_monotonic_ns = time.monotonic_ns()
        return True


class BrowserDriver:
    """Isolated shipping pages; no shared user profile and no write retry."""

    def __init__(self, base_url: str, artifact_dir: Path | str, *, timeout_ms: int = 30000,
                 headless: bool = True, channel: str | None = "chrome",
                 executable_path: str | None = None):
        self.base_url = normalize_base_url(base_url)
        if type(timeout_ms) is not int or not 1000 <= timeout_ms <= 120000:
            raise ValueError("Browser timeout must be between 1000 and 120000 ms")
        self.artifact_dir = Path(artifact_dir)
        self.timeout_ms, self.headless = timeout_ms, headless
        self.channel, self.executable_path = channel, executable_path
        self._playwright = self._browser = None
        self._operation = 0
        self._profile_collisions = {}

    def __enter__(self):
        from playwright.sync_api import sync_playwright
        self.artifact_dir.mkdir(parents=True, exist_ok=True, mode=0o700)
        self._playwright = sync_playwright().start()
        try:
            options = {"headless": self.headless, "timeout": self.timeout_ms}
            if self.executable_path:
                options["executable_path"] = self.executable_path
            elif self.channel:
                options["channel"] = self.channel
            self._browser = self._playwright.chromium.launch(**options)
        except BaseException:
            self._playwright.stop()
            self._playwright = None
            raise
        return self

    def __exit__(self, *_):
        try:
            if self._browser:
                self._browser.close()
        finally:
            if self._playwright:
                self._playwright.stop()
            self._browser = self._playwright = None
            self._profile_collisions.clear()

    def _profile_page(self, label):
        self._operation += 1
        return _ProfilePage(self, f"profile-{self._operation:03d}-{label}-{time.time_ns()}")

    def _profile_finish(self, session, result, *, close=True):
        from behavior_state import atomic_json
        session.drain_responses()
        result["post_count"] = session.guard.forwarded
        posts = [x for x in session.responses if x["method"] == "POST"]
        result["post_status"] = posts[-1]["status"] if posts else None
        result["mutation_outcome"] = ("not_sent" if not session.guard.forwarded else
            "confirmed" if result["post_status"] == 200 else
            "rejected" if result["post_status"] and result["post_status"] >= 400 else "possibly_committed")
        result["blocked_requests"] = session.guard.blocked
        result["browser_errors"] = session.errors
        if session.guard.blocked or session.errors:
            result.update(result="FAIL", reason="browser_profile_contract_failed")
        try:
            session.page.screenshot(path=str(self.artifact_dir / f"{session.label}.png"),
                                    timeout=2000, full_page=True)
            result["screenshot"] = f"{session.label}.png"
        except Exception:
            result["screenshot_unavailable"] = True
        if close:
            try:
                session.close()
            except Exception:
                result["browser_close_error"] = True
                if result["result"] == "PASS":
                    result.update(result="INCOMPLETE", reason="browser_close_unconfirmed")
        result["browser_closed"] = session.closed
        atomic_json(self.artifact_dir / f"{session.label}-responses.json", session.responses)
        atomic_json(self.artifact_dir / f"{session.label}.json", result)
        return result

    def _profile_run(self, label, document, action, *, session=None):
        session = session or self._profile_page(label)
        result = {"result": "INCOMPLETE", "reason": "profile_operation_incomplete", "readback": None}
        try:
            action(session, result)
        except Exception as error:
            result["exception_type"] = type(error).__name__
            if session.guard.forwarded:
                try:
                    result["readback"] = session.read_profile(document["name"], absent=label == "delete")
                except Exception:
                    result["readback_error"] = True
        return self._profile_finish(session, result)

    def prepare_profile_collision(self, document, catalog_names):
        """Keep one naturally stale New dialog open before another page creates it."""
        document = _profile_document(document)
        if not _same_json(document, profile_storage_document(document["name"], document["description"])):
            raise ValueError("New-profile automation supports the shipping defaults only")
        session = self._profile_page("stale")
        try:
            session.open(catalog_names)
            session.draft(document)
        except Exception as error:
            self._profile_finish(session, {"result": "INCOMPLETE", "reason": "stale_draft_not_ready",
                                          "exception_type": type(error).__name__})
            raise
        token = session.label
        self._profile_collisions[token] = (session, document)
        return token

    def discard_profile_collision(self, token):
        value = self._profile_collisions.pop(token, None)
        if value is not None:
            value[0].close()

    def profile_create(self, document, catalog_names):
        document = _profile_document(document)
        if not _same_json(document, profile_storage_document(document["name"], document["description"])):
            raise ValueError("New-profile automation supports the shipping defaults only")

        def action(session, result):
            from playwright.sync_api import expect
            session.open(catalog_names)
            dialog = session.draft(document)
            response = session.send(dialog.get_by_role("button", name="Save", exact=True),
                                    "/api/v1/profile", _profile_payload(document, creating=True))
            if response.status != 200:
                result.update(result="FAIL", reason="profile_create_rejected")
                return
            expect(session.page.get_by_text(f'Profile "{document["name"]}" saved', exact=True)).to_be_visible()
            result["readback"] = session.read_profile(document["name"])
            result.update(result="PASS" if _same_json(result["readback"], document) else "FAIL",
                          reason="profile_create_readback")
        return self._profile_run("create", document, action)

    def profile_edit(self, before_document, after_document):
        before, after = _profile_document(before_document), _profile_document(after_document)
        if not _same_json({**before, "description": after["description"]}, after):
            raise ValueError("Profile edit automation supports description changes only")

        def action(session, result):
            from playwright.sync_api import expect
            session.open()
            editor = session.edit(before)
            editor.get_by_label("Description", exact=True).fill(after["description"])
            response = session.send(editor.get_by_role("button", name="Save Profile", exact=True),
                                    "/api/v1/profile", _profile_payload(after, creating=False))
            if response.status != 200:
                result.update(result="FAIL", reason="profile_edit_rejected")
                return
            expect(session.page.get_by_text(f'Profile "{after["name"]}" saved', exact=True)).to_be_visible()
            result["readback"] = session.read_profile(after["name"])
            result.update(result="PASS" if _same_json(result["readback"], after) else "FAIL",
                          reason="profile_edit_readback")
        return self._profile_run("edit", after, action)

    def profile_collision(self, token):
        session, document = self._profile_collisions.pop(token)

        def action(session, result):
            from playwright.sync_api import expect
            session.drain_responses()
            for item in session.responses:
                if item["path"] == "/api/v1/profiles" and any(
                        row["name"].lower() == document["name"].lower()
                        for row in item.get("body", {}).get("profiles", [])):
                    raise ValueError("Stale page observed the occupied name")
            dialog = session.page.get_by_role("dialog", name="Save Profile")
            response = session.send(dialog.get_by_role("button", name="Save", exact=True),
                                    "/api/v1/profile", _profile_payload(document, creating=True))
            body = session.response_json(response) if response.status == 409 else None
            # The shipping error response also includes a companion message.
            # Only the canonical collision error establishes this scenario.
            if not isinstance(body, dict) or body.get("error") != "Profile already exists":
                result.update(result="FAIL", reason="occupied_profile_not_rejected")
                return
            expect(session.page.get_by_text("Failed to save: Profile already exists", exact=True)).to_be_visible()
            expect(dialog).to_be_visible()
            expect(dialog.get_by_label("Profile Name", exact=True)).to_have_value(document["name"])
            expect(dialog.locator("#profile-description")).to_have_value(document["description"])
            result["readback"] = session.read_profile(document["name"])
            result.update(result="PASS", reason="occupied_create_rejected_draft_retained")
        return self._profile_run("collision", document, action, session=session)

    def profile_reload(self, expected_document):
        document = _profile_document(expected_document)

        def action(session, result):
            session.open()
            session.edit(document)
            result["readback"] = session.read_profile(document["name"])
            result.update(result="PASS" if _same_json(result["readback"], document) else "FAIL",
                          reason="fresh_page_profile_readback")
        return self._profile_run("reload", document, action)

    def profile_delete(self, expected_document):
        document = _profile_document(expected_document)

        def action(session, result):
            session.open()
            editor = session.edit(document)
            editor.get_by_role("button", name="Cancel", exact=True).click()
            if not _same_json(session.read_profile(document["name"]), document):
                raise ValueError("Owned profile changed before delete")
            confirmations = []

            def confirm(dialog):
                accepted = dialog.type == "confirm" and dialog.message == f'Delete profile "{document["name"]}"?'
                confirmations.append(accepted)
                dialog.accept() if accepted else dialog.dismiss()
            session.page.once("dialog", confirm)
            response = session.send(session.row(document["name"]).get_by_role("button", name="Delete", exact=True),
                                    "/api/v1/profile/delete", {"name": document["name"]})
            if response.status != 200 or confirmations != [True]:
                result.update(result="FAIL", reason="profile_delete_not_confirmed")
                return
            result["readback"] = session.read_profile(document["name"], absent=True)
            result.update(result="PASS", reason="owned_profile_deleted")
        return self._profile_run("delete", document, action)

    def save_display(self, patch: dict, expected_before: dict) -> dict:
        """Click Save once; return evidence even when its response is uncertain.

        PASS needs a successful UI terminal message and complete target readback.
        An ambiguous response remains INCOMPLETE even if a read-only followup
        observes the requested values. The caller must still perform its own
        independent device GET and recovery/restoration checks.
        """
        baseline, target = target_display(expected_before, patch)
        if self._browser is None:
            raise RuntimeError("BrowserDriver must be entered before saving")
        self._operation += 1
        prefix = f"save-{self._operation:03d}-{time.time_ns()}"
        guard = SaveWriteGuard(self.base_url, target)
        result = {"result": "INCOMPLETE", "reason": "browser_operation_incomplete",
                  "post_count": 0, "post_status": None, "readback": None,
                  "readback_matches": False, "terminal_message": None,
                  "mutation_outcome": "not_sent"}
        context = self._browser.new_context(service_workers="block", viewport={"width": 1280, "height": 1000})
        page = context.new_page()
        deadline = time.monotonic() + self.timeout_ms / 1000
        observations = {"initial": None, "load_error": None, "maintenance": None,
                        "quiet_loaded": False, "post_readback": None}

        def remaining():
            value = max(1, int((deadline - time.monotonic()) * 1000))
            page.set_default_timeout(value)
            return value

        def route_request(route):
            request = route.request
            if guard.admit(request.method, request.url, request.headers, request.post_data):
                route.continue_()
            else:
                route.abort("blockedbyclient")

        def response_received(response):
            path = urlsplit(response.url).path
            method = response.request.method
            if path == DISPLAY_PATH and method == "POST":
                result["post_status"] = response.status
                result["post_response_monotonic_ns"] = time.monotonic_ns()
            elif method == "GET" and path in (DISPLAY_PATH, "/api/quiet/settings", "/api/status"):
                try:
                    if response.status != 200:
                        raise ValueError("read failed")
                    value = response.json()
                    if path == DISPLAY_PATH:
                        value = canonical_display(value)
                        observations["post_readback" if guard.forwarded else "initial"] = value
                    elif path == "/api/quiet/settings":
                        observations["quiet_loaded"] = type(value.get("stealthEnabled")) is bool
                    else:
                        observations["maintenance"] = value.get("maintenanceBoot") is True
                except Exception:
                    if path == DISPLAY_PATH and not guard.forwarded:
                        observations["load_error"] = "initial_display_read_failed"

        def pump_until(predicate):
            while not predicate():
                if time.monotonic() >= deadline:
                    raise TimeoutError("Browser phase deadline")
                page.wait_for_timeout(min(50, remaining()))

        try:
            context.route("**/*", route_request)
            page.on("response", response_received)
            page.goto(self.base_url + "/colors", wait_until="domcontentloaded", timeout=remaining())
            pump_until(lambda: observations["load_error"] or (
                observations["initial"] is not None and observations["maintenance"] is not None
                and observations["quiet_loaded"]))
            if observations["load_error"]:
                result["reason"] = observations["load_error"]
                return result
            if observations["maintenance"] is not True:
                result["reason"] = "maintenance_mode_not_observed"
                return result
            if observations["initial"] != baseline:
                result["reason"] = "baseline_changed_before_edit"
                return result

            remaining()
            page.get_by_role("button", name="Show all settings", exact=True).click()
            if any(key.startswith("bar") and key[3:].isdigit() for key in patch):
                advanced = page.get_by_role("button", name="Edit all 6 colors individually", exact=True)
                if advanced.count():
                    advanced.click()

            def toggle(key, value):
                title, inverted = TOGGLES[key]
                remaining()
                # Labels include explanatory text for visibility controls.
                locator = page.get_by_role("checkbox", name=re.compile(r"^" + re.escape(title)))
                locator.set_checked(not value if inverted else value)

            if "freq" in patch:
                toggle("freqUseBandColor", False)
            if "showBatteryPercent" in patch:
                toggle("hideBatteryIcon", False)
            for key, value in patch.items():
                remaining()
                if key in COLOR_CONTROLS:
                    control = page.locator(f"#{COLOR_CONTROLS[key]} + input")
                    control.fill(f"{value:04X}")
                    control.press("Tab")  # Commit the real input/change path.
                elif key == "brightness":
                    # Playwright fill excludes range inputs; dispatch the user
                    # input/change events after using its native value setter.
                    page.locator("#brightness-slider").evaluate("""(element, value) => {
                        Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value').set.call(element, String(value));
                        element.dispatchEvent(new Event('input', {bubbles: true}));
                        element.dispatchEvent(new Event('change', {bubbles: true}));
                    }""", value)
            for key in TOGGLES:
                if key == "hideBatteryIcon":
                    continue  # Keep percentage control enabled until it is set.
                if key in patch or (key == "freqUseBandColor" and "freq" in patch) or (
                        key == "hideBatteryIcon" and "showBatteryPercent" in patch):
                    toggle(key, target[key])
            if "hideBatteryIcon" in patch or "showBatteryPercent" in patch:
                toggle("hideBatteryIcon", target["hideBatteryIcon"])
            remaining()
            guard.armed = True
            page.get_by_role("button", name="Save display settings", exact=True).click()

            def terminal():
                if guard.blocked:
                    return True
                for message in TERMINAL_MESSAGES:
                    if page.get_by_text(message, exact=True).is_visible():
                        result["terminal_message"] = message
                        return True
                return False

            pump_until(terminal)
            if guard.blocked:
                result.update(result="FAIL", reason="browser_write_contract_failed")
            elif result["post_status"] is not None and result["post_status"] >= 400:
                result.update(result="FAIL", reason="display_save_rejected", mutation_outcome="rejected")
            elif result["terminal_message"] == SUCCESS_MESSAGE and guard.forwarded == 1:
                result["readback"] = observations["post_readback"]
                result["readback_matches"] = result["readback"] == target
                if result["post_status"] == 200 and result["readback_matches"]:
                    result.update(result="PASS", reason="saved_and_reloaded", mutation_outcome="confirmed")
                else:
                    result.update(result="FAIL", reason="successful_save_readback_mismatch")
            else:
                result["reason"] = "save_response_or_ui_completion_uncertain"
        except Exception as exc:
            # Exceptions can include URLs or device content; store only class.
            result["exception_type"] = type(exc).__name__
        finally:
            result["post_count"] = guard.forwarded
            result["post_started_monotonic_ns"] = guard.sent_monotonic_ns
            result["blocked_requests"] = guard.blocked
            if guard.forwarded and result["mutation_outcome"] == "not_sent":
                result["mutation_outcome"] = "possibly_committed"
            if guard.forwarded and result["readback"] is None:
                # Read-only reconciliation; never click Save a second time.
                try:
                    reply = context.request.get(self.base_url + DISPLAY_PATH, timeout=5000, max_redirects=0)
                    if reply.status == 200:
                        result["readback"] = canonical_display(reply.json())
                        result["readback_matches"] = result["readback"] == target
                    reply.dispose()
                except Exception:
                    result["readback_error"] = True
            if result["result"] != "PASS":
                try:
                    screenshot = self.artifact_dir / f"{prefix}.png"
                    page.screenshot(path=str(screenshot), timeout=2000, full_page=False)
                    result["screenshot"] = screenshot.name
                except Exception:
                    pass
            try:
                context.close()
            except Exception:
                result["browser_close_error"] = True
                if result["result"] == "PASS":
                    result.update(result="INCOMPLETE", reason="browser_close_unconfirmed")
            (self.artifact_dir / f"{prefix}.json").write_text(json.dumps(result, indent=2) + "\n")
        return result
