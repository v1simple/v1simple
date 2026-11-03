"""Durable recovery and bounded device operations for the behavior bench.

The journal is private evidence. It records intent before any setting write and
restores only fields owned by this run. It never imports a complete backup.
"""
from __future__ import annotations

from copy import deepcopy
import hashlib
import json
import os
from pathlib import Path
import re
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request

from behavior_browser import canonical_display, normalize_base_url, target_display

ENDPOINTS = {
    "backup": "/api/settings/backup", "display": "/api/display/settings",
    "quiet": "/api/quiet/settings", "audio": "/api/audio/settings",
    "obd": "/api/obd/config", "gps": "/api/gps/config",
    "slots": "/api/autopush/slots", "device": "/api/device/settings",
    "devices": "/api/v1/devices",
}


class Incomplete(RuntimeError):
    """A required observation or recovery could not be established."""


class BehaviorFailure(RuntimeError):
    """A complete observation contradicted the requested behavior."""


class DeviceHttpError(Incomplete):
    """Bounded HTTP rejection details suitable for the private run record."""

    def __init__(self, endpoint: str, status: int, payload: object):
        self.status = status
        self.profile_storage_busy = status == 409 and payload == {"error": "Profile storage busy; retry"}
        self.payload = {}
        if isinstance(payload, dict):
            for key in ("error", "message", "success", "retryable"):
                value = payload.get(key)
                if type(value) is str:
                    self.payload[key] = value[:240]
                elif type(value) is bool:
                    self.payload[key] = value
        # Never include the profile-name query or unrelated response content.
        detail = json.dumps(self.payload, ensure_ascii=True, separators=(",", ":"))
        super().__init__(f"HTTP {status} {urllib.parse.urlsplit(endpoint).path}: {detail}")


def atomic_json(path: Path, value: object) -> None:
    path = Path(path)
    if path.is_symlink():
        raise Incomplete("Refusing a linked recovery file")
    # A killed host may leave its temporary file behind. A unique sibling lets
    # recovery proceed without deleting another writer's unfinished evidence.
    fd, name = tempfile.mkstemp(prefix=path.name + ".", suffix=".tmp", dir=path.parent)
    temporary = Path(name)
    try:
        with os.fdopen(fd, "w") as stream:
            json.dump(value, stream, indent=2, sort_keys=True, allow_nan=False)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
        directory = os.open(path.parent, os.O_RDONLY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    finally:
        temporary.unlink(missing_ok=True)


def digest(value: object) -> str:
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(",", ":"),
                                    allow_nan=False).encode()).hexdigest()


def differences(left, right, prefix="") -> list[str]:
    if type(left) is not type(right):
        return [prefix]
    if isinstance(left, dict):
        return [item for key in sorted(left.keys() | right.keys())
                for item in ([f"{prefix}.{key}".strip(".")] if key not in left or key not in right
                             else differences(left[key], right[key], f"{prefix}.{key}".strip(".")))]
    if isinstance(left, list):
        if len(left) != len(right):
            return [prefix + ".length"]
        return [item for index, (a, b) in enumerate(zip(left, right))
                for item in differences(a, b, f"{prefix}[{index}]")]
    return [] if left == right else [prefix]


def semantic_snapshot(snapshot: dict) -> dict:
    validate_snapshot(snapshot)
    result = deepcopy(snapshot)
    backup = result["backup"]
    for key in ("_timestamp", "timestamp"):
        backup.pop(key, None)
    backup["profiles"].sort(key=lambda row: row["name"])
    result["display"] = canonical_display(result["display"])
    # The transaction selects alternating NVS namespaces; their contents still
    # have to match. Catalog order is immaterial, but connected state is kept.
    result["device"]["nvsDiag"].pop("ns")
    result["devices"]["devices"].sort(key=lambda row: row["address"])
    return result


def validate_slots(slots: object) -> None:
    if (not isinstance(slots, dict) or type(slots.get("schemaVersion")) is not int
            or slots["schemaVersion"] != 4 or slots.get("detectorConfigurationOwner") != "profile"
            or type(slots.get("activeSlot")) is not int or slots["activeSlot"] not in range(3)
            or type(slots.get("enabled")) is not bool or not isinstance(slots.get("slots"), list)
            or len(slots["slots"]) != 3):
        raise Incomplete("Incomplete profile-owned slot snapshot")
    for row in slots["slots"]:
        if (not isinstance(row, dict) or not isinstance(row.get("profile"), str)
                or not isinstance(row.get("name"), str)
                or any(type(row.get(key)) is not bool for key in (
                    "volumeConfigured", "darkModeConfigured", "darkMode", "priorityArrowOnly"))
                or any(type(row.get(key)) is not int or not low <= row[key] <= high
                       for key, low, high in (("color", 1, 65535), ("volume", 0, 9),
                                              ("muteVolume", 0, 9), ("alertPersist", 0, 5)))):
            raise Incomplete("Invalid profile-owned slot configuration")


def validate_snapshot(snapshot: dict) -> None:
    if not isinstance(snapshot, dict) or set(snapshot) != set(ENDPOINTS):
        raise Incomplete("Incomplete configuration snapshot")
    for key in ENDPOINTS:
        if not isinstance(snapshot[key], dict) or not snapshot[key] or "error" in snapshot[key]:
            raise Incomplete("Incomplete configuration response: " + key)
    try:
        canonical_display(snapshot["display"])
        digest(snapshot)  # Reject non-JSON and non-finite values before journaling.
    except (TypeError, ValueError) as exc:
        raise Incomplete("Invalid configuration snapshot") from exc
    backup = snapshot["backup"]
    if (not isinstance(backup, dict) or backup.get("_type") != "v1simple_backup"
            or type(backup.get("_version")) is not int or not isinstance(backup.get("profiles"), list)):
        raise Incomplete("Unsupported complete backup response")
    names = [row.get("name") if isinstance(row, dict) else None for row in backup["profiles"]]
    if any(not isinstance(name, str) or not name for name in names) or len(set(names)) != len(names):
        raise Incomplete("Invalid backup profile catalog")
    validate_slots(snapshot["slots"])
    device = snapshot["device"]
    if (any(type(device.get(key)) is not str for key in ("ap_ssid", "ap_password", "proxy_name"))
            or any(type(device.get(key)) is not bool for key in (
                "isDefaultPassword", "proxy_ble", "alpEnabled", "alpDisableV1LaserOnPush"))
            or any(type(device.get(key)) is not int or device[key] < 0 for key in (
                "autoPowerOffMinutes", "apTimeoutMinutes", "alpAlertPersistSec"))):
        raise Incomplete("Incomplete device settings response")
    diag = device.get("nvsDiag")
    if (not isinstance(diag, dict) or not isinstance(diag.get("ns"), str) or not diag["ns"]
            or any(type(diag.get(key)) is not int for key in ("valid", "ver", "bright"))
            or any(type(diag.get(key)) is not bool for key in ("proxy", "autoPush", "healthy"))):
        raise Incomplete("Incomplete flash settings diagnostic")
    catalog = snapshot["devices"]
    rows = catalog.get("devices")
    if (not isinstance(rows, list) or type(catalog.get("count")) is not int or catalog["count"] != len(rows)
            or any(not isinstance(row, dict) or not isinstance(row.get("address"), str)
                   or not row["address"] or not isinstance(row.get("name"), str)
                   or type(row.get("defaultProfile")) is not int or row["defaultProfile"] not in range(4)
                   or type(row.get("connected")) is not bool for row in rows)):
        raise Incomplete("Incomplete known-device catalog")
    if len({row["address"] for row in rows}) != len(rows):
        raise Incomplete("Duplicate known-device identity")


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        raise Incomplete("Device HTTP redirect refused")


class HttpDevice:
    def __init__(self, base_url: str, *, timeout_s: float = 8):
        from behavior_browser import normalize_base_url
        self.base_url = normalize_base_url(base_url)
        if not isinstance(timeout_s, (int, float)) or not 0 < timeout_s <= 60:
            raise ValueError("HTTP timeout must be between zero and 60 seconds")
        self.timeout_s = timeout_s
        # Device requests must go directly to the pinned origin, never an
        # environment-configured proxy or an HTTP redirect target.
        self.opener = urllib.request.build_opener(urllib.request.ProxyHandler({}), _NoRedirect())

    def request(self, endpoint: str, fields: dict | None = None, *, timeout_s: float | None = None) -> dict:
        if not isinstance(endpoint, str):
            raise ValueError("Invalid device API path")
        parsed = urllib.parse.urlsplit(endpoint)
        if not re.fullmatch(r"/api/[a-zA-Z0-9/_-]+", parsed.path) or parsed.scheme or parsed.netloc or parsed.fragment:
            raise ValueError("Invalid device API path")
        if endpoint != parsed.path:
            from usb_profiles import canonical_name
            try:
                pairs = urllib.parse.parse_qsl(parsed.query, strict_parsing=True, keep_blank_values=True,
                                              errors="strict", max_num_fields=1)
            except (ValueError, UnicodeError) as exc:
                raise ValueError("Invalid device API query") from exc
            if (fields is not None or parsed.path != "/api/v1/profile" or len(pairs) != 1
                    or pairs[0][0] != "name" or not canonical_name(pairs[0][1])
                    or endpoint != parsed.path + "?" + urllib.parse.urlencode(pairs)):
                raise ValueError("Invalid device API query")
        timeout = self.timeout_s if timeout_s is None else min(timeout_s, self.timeout_s)
        if not 0 < timeout <= 60:
            raise ValueError("Invalid remaining HTTP read budget")
        headers = {"Cache-Control": "no-store"}
        body = None
        if fields is not None:
            headers.update({"X-V1Simple-Request": "maintenance-ui",
                            "Content-Type": "application/x-www-form-urlencoded"})
            body = urllib.parse.urlencode({k: str(v).lower() if type(v) is bool else str(v)
                                          for k, v in fields.items()}).encode()
        req = urllib.request.Request(self.base_url + endpoint, data=body, headers=headers)
        try:
            with self.opener.open(req, timeout=timeout) as response:
                raw = response.read(524289)
                status = getattr(response, "status", 200)
        except urllib.error.HTTPError as exc:
            with exc:
                raw = exc.read(2049)
            try:
                payload = json.loads(raw) if len(raw) <= 2048 else {}
            except (ValueError, UnicodeError):
                payload = {}
            raise DeviceHttpError(endpoint, exc.code, payload) from exc
        if len(raw) > 524288:
            raise Incomplete("Device response exceeds read bound")
        value = json.loads(raw)
        if status != 200 or not isinstance(value, dict) or "error" in value or value.get("success") is False:
            raise DeviceHttpError(endpoint, status, value)
        return value

    def snapshot(self) -> dict:
        value = {key: self.request(endpoint) for key, endpoint in ENDPOINTS.items()}
        validate_snapshot(value)
        return value

    def display(self) -> dict:
        return canonical_display(self.request(ENDPOINTS["display"]))

    def wait_profile_readable(self, name: str, timeout_s: float = 30) -> dict:
        """Poll only an explicitly busy profile GET; never retry a mutation."""
        from usb_profiles import (ProfileError, canonical_name, validate_detector,
                                  validate_in_the_box)
        if not canonical_name(name) or type(timeout_s) not in (int, float) or not 0 < timeout_s <= 60:
            raise ValueError("Invalid profile readiness request")
        endpoint = "/api/v1/profile?" + urllib.parse.urlencode({"name": name})
        deadline = time.monotonic() + timeout_s
        last = None
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                detail = f": {last}" if last is not None else ""
                raise Incomplete("Profile readiness deadline exceeded" + detail)
            try:
                value = self.request(endpoint, timeout_s=remaining)
            except DeviceHttpError as exc:
                if not exc.profile_storage_busy:
                    raise
                last = exc
                remaining = deadline - time.monotonic()
                if remaining > 0:
                    time.sleep(min(.25, remaining))
                continue
            # The deadline is fixed, including the successful read. A delayed
            # response must not authorize an overdue write.
            if time.monotonic() >= deadline:
                raise Incomplete("Profile readiness response exceeded the read budget")
            if (type(value.get("schemaVersion")) is not int or value["schemaVersion"] != 4
                    or value.get("name") != name or not isinstance(value.get("description"), str)
                    or not isinstance(value.get("settings"), dict)):
                raise Incomplete("Incomplete or mismatched profile readiness response")
            raw = value["settings"].get("bytes")
            if (not isinstance(raw, list) or len(raw) != 6
                    or any(type(item) is not int or not 0 <= item <= 255 for item in raw)):
                raise Incomplete("Incomplete profile readiness settings")
            try:
                validate_detector(value.get("detector"), 4)
                validate_in_the_box(value.get("inTheBox"))
            except (ProfileError, KeyError, TypeError, ValueError) as exc:
                raise Incomplete("Invalid profile readiness schema") from exc
            return value

    def persistence(self, slot: int, expected_before: dict, value: int) -> None:
        validate_persistence_intent({"slot": slot, "value": value})
        validate_slots(expected_before)
        profile = expected_before["slots"][slot]["profile"]
        if profile:
            self.wait_profile_readable(profile)
        # The profile read may have waited for the deferred SD backup. Compare
        # all slot fields again after that wait, before the sole POST.
        current = self.request(ENDPOINTS["slots"])
        validate_slots(current)
        if current != expected_before:
            raise Incomplete("Slot configuration changed before the requested write")
        row = current["slots"][slot]
        fields = {"slot": slot, "profile": row["profile"], "alertPersist": value}
        if not row["profile"]:
            fields["clearProfile"] = True
        # At most one write. A lost response is resolved by a separate complete read.
        error = None
        try:
            self.request("/api/autopush/slot", fields)
        except Exception as exc:
            error = exc
        try:
            actual = self.request(ENDPOINTS["slots"])
            validate_slots(actual)
        except Exception as exc:
            detail = f"; write response: {error}" if error is not None else ""
            raise Incomplete("Persistence readback unavailable" + detail) from exc
        expected = deepcopy(current)
        expected["slots"][slot]["alertPersist"] = value
        if actual != expected:
            if error is not None:
                raise Incomplete("Persistence write outcome could not be confirmed: " + str(error)) from error
            raise BehaviorFailure("Saved persistence did not match authoritative readback")

    def preview_start(self) -> tuple[int, int]:
        self.request("/api/display/preview/clear", {})
        before = time.monotonic_ns()
        value = self.request("/api/display/preview", {})
        after = time.monotonic_ns()
        if value.get("active") is not True:
            raise Incomplete("A fresh preview was not acknowledged")
        return before, after


class RecoveryJournal:
    def __init__(self, path: Path, value: dict):
        self.path, self.value = Path(path), value

    @classmethod
    def create(cls, directory: Path, identity: dict, baseline: dict, profiles: dict, usb_identity,
               *, connection: dict | None = None) -> "RecoveryJournal":
        value = deepcopy({"schema": 1, "identity": identity, "usb_identity": usb_identity,
                 "baseline": baseline, "profiles": profiles,
                 "baseline_digest": digest({"baseline": baseline, "profiles": profiles}),
                 "display_intents": [], "persistence_intents": [],
                 "restoration": "not_needed", "stage": "backed_up"})
        if connection is not None:
            value["connection"] = deepcopy(connection)
        validate_journal(value)
        path = Path(directory) / "recovery.json"
        if path.exists():
            raise Incomplete("Recovery journal already exists")
        atomic_json(path, value)
        return cls(path, value)

    @classmethod
    def load(cls, directory: Path) -> "RecoveryJournal":
        path = Path(directory) / "recovery.json"
        if path.is_symlink():
            raise Incomplete("Refusing a linked recovery journal")
        if path.stat().st_size > 4 * 1024 * 1024:
            raise Incomplete("Recovery journal exceeds read bound")
        try:
            value = json.loads(path.read_text())
        except (ValueError, UnicodeError) as exc:
            raise Incomplete("Unreadable recovery journal") from exc
        validate_journal(value)
        return cls(path, value)

    def save(self, **updates) -> None:
        candidate = deepcopy({**self.value, **updates})
        validate_journal(candidate)
        atomic_json(self.path, candidate)
        self.value = candidate

    def display_intent(self, patch: dict) -> None:
        target_display(self.value["baseline"]["display"], patch)
        self.save(display_intents=[*self.value["display_intents"], deepcopy(patch)],
                  restoration="required", stage="display_write_pending")

    def persistence_intent(self, slot: int, value: int) -> None:
        intent = {"slot": slot, "value": value}
        validate_persistence_intent(intent)
        self.save(persistence_intents=[*self.value["persistence_intents"], intent],
                  restoration="required", stage="persistence_write_pending")

    def _display_owned_values(self, current: dict, keys) -> dict:
        validate_journal(self.value)
        current = canonical_display(current)
        original = canonical_display(self.value["baseline"]["display"])
        allowed = {key: [original[key]] for key in keys}
        for intent in self.value["display_intents"]:
            for key, value in intent.items():
                if key in allowed:
                    allowed[key].append(value)
        for key, values in allowed.items():
            if current[key] not in values:
                raise Incomplete("A tested display field has an unowned value; refusing overwrite")
        return original

    def validate_display_change(self, current: dict, patch: dict) -> None:
        """Refuse a newer value before first ownership is recorded for a field."""
        target_display(current, patch)
        self._display_owned_values(current, patch.keys())

    def display_restore_patch(self, current: dict) -> dict:
        validate_journal(self.value)
        keys = {key for intent in self.value["display_intents"] for key in intent}
        original = self._display_owned_values(current, keys)
        return {key: original[key] for key in keys if current[key] != original[key]}

    def _persistence_owned_values(self, current: dict, slots) -> dict:
        validate_journal(self.value)
        validate_slots(current)
        baseline = self.value["baseline"]["slots"]
        if (current.get("activeSlot") != baseline["activeSlot"] or current.get("enabled") != baseline["enabled"]
                or len(current.get("slots", [])) != 3):
            raise Incomplete("Slot identity changed during the test")
        allowed = {slot: [baseline["slots"][slot]["alertPersist"]] for slot in slots}
        for intent in self.value["persistence_intents"]:
            slot = intent["slot"]
            if slot in allowed:
                allowed[slot].append(intent["value"])
        for slot, values in allowed.items():
            if current["slots"][slot]["profile"] != baseline["slots"][slot]["profile"]:
                raise Incomplete("Profile assignment changed; refusing restoration write")
            if current["slots"][slot]["alertPersist"] not in values:
                raise Incomplete("Persistence has an unowned value; refusing overwrite")
        return baseline

    def validate_persistence_change(self, current: dict, slot: int) -> None:
        validate_persistence_intent({"slot": slot, "value": 0})
        self._persistence_owned_values(current, [slot])

    def persistence_restore(self, current: dict) -> list[tuple[int, int]]:
        validate_journal(self.value)
        slots = sorted({intent["slot"] for intent in self.value["persistence_intents"]})
        baseline = self._persistence_owned_values(current, slots)
        return [(slot, baseline["slots"][slot]["alertPersist"]) for slot in slots
                if current["slots"][slot]["alertPersist"] != baseline["slots"][slot]["alertPersist"]]


def validate_persistence_intent(intent: object) -> None:
    if (not isinstance(intent, dict) or set(intent) != {"slot", "value"}
            or type(intent["slot"]) is not int or intent["slot"] not in range(3)
            or type(intent["value"]) is not int or intent["value"] not in range(6)):
        raise Incomplete("Invalid persistence intent")


def validate_firmware_identity(value: object) -> None:
    if not isinstance(value, dict) or any(
            type(value.get(key)) is not str or not re.fullmatch(r"[0-9a-fA-F]{7,64}", value[key])
            for key in ("git", "image")):
        raise Incomplete("Invalid pinned firmware identity")


def validate_journal(value: object) -> None:
    if (not isinstance(value, dict) or type(value.get("schema")) is not int or value["schema"] != 1
            or not {"identity", "usb_identity", "baseline", "profiles", "baseline_digest",
                    "display_intents", "persistence_intents", "restoration", "stage"} <= value.keys()):
        raise Incomplete("Unsupported or incomplete recovery journal")
    if "connection" in value:
        connection = value["connection"]
        if (not isinstance(connection, dict) or set(connection) != {"base_url", "ssid"}
                or not isinstance(connection["base_url"], str)
                or not isinstance(connection["ssid"], str) or not connection["ssid"]
                or len(connection["ssid"].encode("utf-8")) > 32):
            raise Incomplete("Invalid saved maintenance connection")
        try:
            if normalize_base_url(connection["base_url"]) != connection["base_url"]:
                raise ValueError("non-canonical origin")
        except ValueError as exc:
            raise Incomplete("Invalid saved maintenance origin") from exc
    validate_firmware_identity(value["identity"])
    if value["identity"].get("mode") not in ("normal", "maintenance"):
        raise Incomplete("Invalid original DUT mode")
    usb_identity = value["usb_identity"]
    if (not isinstance(usb_identity, (list, tuple)) or len(usb_identity) != 4
            or usb_identity[0] not in ("serial_number", "location", "device")
            or not isinstance(usb_identity[1], str) or not usb_identity[1]
            or any(type(item) is not int or not 0 <= item <= 65535 for item in usb_identity[2:])):
        raise Incomplete("Missing physical USB identity")
    validate_snapshot(value["baseline"])
    from usb_profiles import ProfileError, validate_bundle
    try:
        validate_bundle(value["profiles"])
    except (ProfileError, TypeError, ValueError, KeyError) as exc:
        raise Incomplete("Invalid complete USB profile backup") from exc
    if digest({"baseline": value["baseline"], "profiles": value["profiles"]}) != value["baseline_digest"]:
        raise Incomplete("Recovery baseline integrity mismatch")
    if not isinstance(value["display_intents"], list) or not isinstance(value["persistence_intents"], list):
        raise Incomplete("Invalid recovery intent lists")
    for patch in value["display_intents"]:
        try:
            target_display(value["baseline"]["display"], patch)
        except (TypeError, ValueError) as exc:
            raise Incomplete("Invalid display recovery intent") from exc
    for intent in value["persistence_intents"]:
        validate_persistence_intent(intent)
    if (value["restoration"] not in ("not_needed", "required", "verified", "unverified")
            or not isinstance(value["stage"], str) or not value["stage"]):
        raise Incomplete("Invalid recovery stage")


def verify_identity(actual: dict, expected: dict) -> None:
    validate_firmware_identity(actual)
    validate_firmware_identity(expected)
    if any(actual.get(key) != expected.get(key) for key in ("git", "image")):
        raise Incomplete("DUT firmware identity does not match the pinned candidate")


def result_status(checks: list[dict], *, completed: bool, restoration: str, error: str | None = None) -> str:
    if any(row.get("result") == "FAIL" for row in checks):
        return "FAIL"
    if not completed or restoration != "verified" or error or not checks:
        return "INCOMPLETE"
    return "PASS" if all(row.get("result") == "PASS" for row in checks) else "INCOMPLETE"
