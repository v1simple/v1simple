"""One guarded browser Save through the shipping Display page.

The caller owns the recovery journal, USB identity, maintenance admission and
independent device readback. This driver never retries a write, restores data,
changes modes, or starts a preview. Playwright is imported only on entry.
"""
from __future__ import annotations

import json
import re
import time
from pathlib import Path
from urllib.parse import parse_qsl, urlsplit


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
    """Fresh isolated page per Save; no shared user profile and no write retry."""

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
