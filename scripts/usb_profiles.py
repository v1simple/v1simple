#!/usr/bin/env python3
"""Private profile backup and verified replacement over the V1Simple USB CDC API."""
from __future__ import annotations

import argparse
from contextlib import contextmanager
from copy import deepcopy
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
import secrets
import sys
import time
import zlib

MAX_PAYLOAD = 128 * 1024
MAX_DESCRIPTION_BYTES = 4096
MAX_PROFILE_COUNT = 10
BUNDLE_VERSION = 4
CHUNK = 64
PREFIX = b"@V1USB1 "
SLOT_KEYS_V1 = {"name", "profile", "mode", "color", "volumeConfigured", "volume", "muteVolume",
                "darkMode", "muteToZero", "alertPersist", "priorityArrowOnly"}
SLOT_KEYS_VERSIONED = {"name", "profile", "color", "alertPersist", "priorityArrowOnly"}
SLOT_MODIFIER_DEFAULTS = {"volumeOverride": False, "volume": 255, "muteVolume": 255,
                          "darkModeOverride": False, "darkMode": False}
SLOT_KEYS_CURRENT = SLOT_KEYS_VERSIONED | SLOT_MODIFIER_DEFAULTS.keys()
PROFILE_KEYS_V1 = {"name", "description", "rawBytes", "displayOn", "mainVolume", "mutedVolume"}
PROFILE_KEYS_VERSIONED = {"schemaVersion", "name", "description", "rawBytes", "detector"}
ASCII_UPPER = str.maketrans("abcdefghijklmnopqrstuvwxyz", "ABCDEFGHIJKLMNOPQRSTUVWXYZ")
ASCII_LOWER = str.maketrans("ABCDEFGHIJKLMNOPQRSTUVWXYZ", "abcdefghijklmnopqrstuvwxyz")


class ProfileError(Exception):
    """A user-visible error containing no profile content or raw serial data."""


class TransportError(ProfileError):
    pass


class DeviceError(ProfileError):
    pass


def require(condition, message):
    if not condition:
        raise ProfileError(message)


def integer(value, low, high):
    return type(value) is int and low <= value <= high


def text_within(value, limit):
    try:
        json_safe_controls = {8, 9, 10, 12, 13}
        return (type(value) is str and
                not any(ord(char) < 32 and ord(char) not in json_safe_controls for char in value) and
                len(value.encode("utf-8")) <= limit)
    except UnicodeError:
        return False


def canonical_name(value):
    return (text_within(value, 64) and bool(value) and value == value.strip(" \t\r\n\v\f")
            and value[0] not in "._" and ".." not in value
            and not any(ord(char) < 32 or ord(char) == 127 or char in '/\\:*?"<>|' for char in value))


def utf8_prefix(value, max_bytes):
    """Return the longest whole-code-point prefix within a byte budget."""
    require(type(value) is str and type(max_bytes) is int and max_bytes >= 0,
            "Invalid migrated profile name")
    encoded = value.encode("utf-8")
    if len(encoded) <= max_bytes:
        return value
    return encoded[:max_bytes].decode("utf-8", errors="ignore")


def migrated_name_candidate(stem, fixed_suffix, collision_ordinal=1):
    """Match firmware migration naming while preserving all deterministic suffixes."""
    require(type(stem) is str and type(fixed_suffix) is str and
            type(collision_ordinal) is int and 1 <= collision_ordinal < 1000,
            "Invalid migrated profile name")
    stem = stem.strip() or "Auto-Push profile"
    collision_suffix = "" if collision_ordinal == 1 else f" #{collision_ordinal}"
    suffix_bytes = len((fixed_suffix + collision_suffix).encode("utf-8"))
    require(suffix_bytes < 64, "Invalid migrated profile name suffix")
    candidate = utf8_prefix(stem, 64 - suffix_bytes) + fixed_suffix + collision_suffix
    require(canonical_name(candidate), "Invalid profile name")
    return candidate


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, "JSON contains duplicate keys")
        result[key] = value
    return result


def parse_json(raw):
    try:
        return json.loads(raw.decode("utf-8"), object_pairs_hook=unique_object,
                          parse_constant=lambda _: (_ for _ in ()).throw(ValueError()))
    except (UnicodeError, ValueError, RecursionError) as exc:
        raise ProfileError("Invalid JSON document") from exc


def validate_mode(value):
    require(type(value) is dict and set(value) in ({"policy"}, {"policy", "value"}),
            "Invalid detector mode")
    require(value["policy"] in ("unchanged", "value"), "Invalid detector mode policy")
    require((value["policy"] == "unchanged" and set(value) == {"policy"}) or
            (value["policy"] == "value" and set(value) == {"policy", "value"}
             and integer(value["value"], 1, 3)), "Invalid detector mode value")


def validate_detector(detector, version):
    require(type(detector) is dict and set(detector) == {
        "userSettings", "mode", "display", "volume", "bluetoothLed", "customFrequencies"},
        "Invalid detector configuration fields")
    require(detector["userSettings"] in ("unchanged", "value") and
            detector["display"] in ("unchanged", "on", "off"),
            "Invalid detector policy")
    validate_mode(detector["mode"])
    volume = detector["volume"]
    require(type(volume) is dict and type(volume.get("policy")) is str,
            "Invalid detector volume")
    if volume["policy"] == "unchanged":
        require(set(volume) == {"policy"}, "Invalid unchanged volume")
    elif volume["policy"] in ("temporary", "saved"):
        keys = {"policy", "main", "muted"} if version == 2 else {
            "policy", "main", "muted", "feedback", "disconnect"}
        require(set(volume) == keys and integer(volume["main"], 0, 9) and
                integer(volume["muted"], 0, 9), "Invalid detector volume values")
        if version == 3:
            require(volume["feedback"] in ("none", "changed_only", "always") and
                    volume["disconnect"] in ("restore_saved", "keep_current") and
                    not (volume["policy"] == "saved" and volume["disconnect"] != "restore_saved"),
                    "Invalid detector volume command policy")
    else:
        raise ProfileError("Invalid detector volume policy")
    if version == 2:
        require(detector["bluetoothLed"] == "unchanged" and
                detector["customFrequencies"] == "unchanged",
                "Invalid schema-v2 detector policy")
        return
    require(detector["bluetoothLed"] in ("unchanged", "off", "on"),
            "Invalid Bluetooth indicator policy")
    custom = detector["customFrequencies"]
    require(type(custom) is dict and type(custom.get("policy")) is str,
            "Invalid custom-frequency policy")
    if custom["policy"] == "unchanged":
        require(set(custom) == {"policy"}, "Invalid unchanged custom-frequency policy")
        return
    require(custom["policy"] == "value" and set(custom) == {"policy", "definitions"}
            and type(custom["definitions"]) is list and 1 <= len(custom["definitions"]) <= 64,
            "Invalid custom-frequency definitions")
    for index, definition in enumerate(custom["definitions"]):
        require(type(definition) is dict and set(definition) == {"index", "lowerMHz", "upperMHz"}
                and definition["index"] == index and integer(definition["lowerMHz"], 0, 65535)
                and integer(definition["upperMHz"], 0, 65535),
                "Invalid custom-frequency definition")
        unused = definition["lowerMHz"] == definition["upperMHz"] == 0
        require(unused or 0 < definition["lowerMHz"] < definition["upperMHz"],
                "Invalid custom-frequency range")


def validate_bundle(bundle):
    require(type(bundle) is dict and set(bundle) == {
        "format", "version", "autoPushEnabled", "activeSlot", "slots", "profiles"},
        "Profile bundle has missing or unknown fields")
    require(bundle["format"] == "v1simple-profiles" and type(bundle["version"]) is int
            and bundle["version"] in (1, 2, 3, BUNDLE_VERSION), "Unsupported profile bundle format/version")
    require(type(bundle["autoPushEnabled"]) is bool and integer(bundle["activeSlot"], 0, 2),
            "Invalid profile bundle state")
    require(type(bundle["slots"]) is list and len(bundle["slots"]) == 3,
            "Profile bundle must contain exactly three slots")
    require(type(bundle["profiles"]) is list, "Profile catalog must be an array")
    require(len(bundle["profiles"]) <= MAX_PROFILE_COUNT,
            f"Profile catalog supports at most {MAX_PROFILE_COUNT} profiles")
    version = bundle["version"]
    profile_version = 3 if version == BUNDLE_VERSION else version
    names, folded_names = set(), set()
    for profile in bundle["profiles"]:
        expected = PROFILE_KEYS_V1 if version == 1 else PROFILE_KEYS_VERSIONED
        require(type(profile) is dict and set(profile) == expected, "Invalid profile fields")
        name = profile["name"]
        require(canonical_name(name), "Invalid profile name")
        require(name.translate(ASCII_LOWER) not in folded_names, "Duplicate profile names")
        names.add(name)
        folded_names.add(name.translate(ASCII_LOWER))
        require(text_within(profile["description"], MAX_DESCRIPTION_BYTES), "Invalid profile description")
        require(type(profile["rawBytes"]) is list and len(profile["rawBytes"]) == 6
                and all(integer(value, 0, 255) for value in profile["rawBytes"]), "Invalid profile bytes")
        if version == 1:
            require(type(profile["displayOn"]) is bool, "Invalid profile display setting")
            require(all(integer(profile[key], 0, 9) or type(profile[key]) is int and profile[key] == 255
                        for key in ("mainVolume", "mutedVolume")), "Invalid profile volume")
        else:
            require(profile["schemaVersion"] == profile_version, "Profile schema does not match bundle version")
            validate_detector(profile["detector"], profile_version)
    for slot in bundle["slots"]:
        expected = (SLOT_KEYS_V1 if version == 1 else
                    SLOT_KEYS_CURRENT if version == BUNDLE_VERSION else SLOT_KEYS_VERSIONED)
        require(type(slot) is dict and set(slot) == expected, "Invalid slot fields")
        require(text_within(slot["name"], 20) and slot["name"] == slot["name"].translate(ASCII_UPPER),
                "Invalid slot name")
        require(type(slot["profile"]) is str and (slot["profile"] == "" or slot["profile"] in names),
                "Slot references an absent profile")
        require(integer(slot["color"], 0, 65535) and integer(slot["alertPersist"], 0, 5)
                and type(slot["priorityArrowOnly"]) is bool, "Invalid slot presentation")
        if version == BUNDLE_VERSION:
            require(slot["color"] != 0, "Invalid slot presentation")
            require(all(type(slot[key]) is bool for key in (
                "volumeOverride", "darkModeOverride", "darkMode")), "Invalid slot modifier boolean")
            require(all(integer(slot[key], 0, 9) if slot["volumeOverride"] else
                        type(slot[key]) is int and slot[key] == 255
                        for key in ("volume", "muteVolume")), "Invalid slot override volume")
            require(slot["darkModeOverride"] or not slot["darkMode"], "Invalid slot dark-mode override")
        if version == 1:
            require(integer(slot["mode"], 0, 3), "Invalid slot mode")
            require(all(type(slot[key]) is bool for key in (
                "volumeConfigured", "darkMode", "muteToZero")), "Invalid slot boolean")
            require(all(integer(slot[key], 0, 9) for key in ("volume", "muteVolume")),
                    "Invalid slot volume")
            require(slot["volumeConfigured"] or slot["volume"] == slot["muteVolume"] == 0,
                    "Unconfigured slot volumes must both be zero")
    return bundle


def default_detector(user_settings="value"):
    return {"userSettings": user_settings, "mode": {"policy": "unchanged"},
            "display": "unchanged", "volume": {"policy": "unchanged"},
            "bluetoothLed": "unchanged", "customFrequencies": {"policy": "unchanged"}}


def migrate_detector_v2(detector):
    migrated = deepcopy(detector)
    volume = migrated["volume"]
    if volume["policy"] != "unchanged":
        volume["feedback"] = "none"
        volume["disconnect"] = "restore_saved"
    migrated["bluetoothLed"] = "off" if migrated["display"] == "off" else "unchanged"
    migrated["customFrequencies"] = {"policy": "unchanged"}
    return migrated


def migrate_bundle(bundle):
    validate_bundle(bundle)
    if bundle["version"] == BUNDLE_VERSION:
        return deepcopy(bundle)
    if bundle["version"] in (2, 3):
        migrated = deepcopy(bundle)
        migrated["version"] = BUNDLE_VERSION
        if bundle["version"] == 2:
            for profile in migrated["profiles"]:
                profile["schemaVersion"] = 3
                profile["detector"] = migrate_detector_v2(profile["detector"])
        for slot in migrated["slots"]:
            slot.update(SLOT_MODIFIER_DEFAULTS)
        validate_bundle(migrated)
        return migrated

    original = deepcopy(bundle)
    catalog = []
    by_name = {}
    for profile in original["profiles"]:
        migrated = {"schemaVersion": 3, "name": profile["name"],
                    "description": profile["description"], "rawBytes": list(profile["rawBytes"]),
                    "detector": default_detector()}
        catalog.append(migrated)
        by_name[migrated["name"]] = migrated
    variants = []
    assigned = []

    def application(profile):
        return profile["rawBytes"], profile["detector"]

    def available(name):
        key = name.translate(ASCII_LOWER)
        return all(entry["name"].translate(ASCII_LOWER) != key for entry in catalog)

    def unique(stem, fixed_suffix):
        for suffix in range(1, 1000):
            candidate = migrated_name_candidate(stem, fixed_suffix, suffix)
            if available(candidate):
                return candidate
        raise ProfileError("Could not assign a deterministic migrated profile name")

    seen_sources = set()
    for slot_index, slot in enumerate(original["slots"]):
        source = by_name.get(slot["profile"])
        if source:
            effective = deepcopy(source)
            if slot["muteToZero"]:
                effective["rawBytes"][0] &= ~0x10
            else:
                effective["rawBytes"][0] |= 0x10
            effective["detector"] = default_detector()
        else:
            effective = {"schemaVersion": 3, "name": "Default", "description": "",
                         "rawBytes": [255] * 6, "detector": default_detector("unchanged")}
        if slot["mode"]:
            effective["detector"]["mode"] = {"policy": "value", "value": slot["mode"]}
        effective["detector"]["display"] = "off" if slot["darkMode"] else "on"
        if slot["darkMode"]:
            effective["detector"]["bluetoothLed"] = "off"
        if slot["volumeConfigured"]:
            effective["detector"]["volume"] = {
                "policy": "temporary", "main": slot["volume"], "muted": slot["muteVolume"],
                "feedback": "none", "disconnect": "restore_saved"}
        source_name = source["name"] if source else ""
        reused = next((entry for entry in variants if entry[0] == source_name and
                       application(entry[2]) == application(effective)), None)
        if reused:
            assigned.append(reused[1])
            continue
        if source and source_name not in seen_sources:
            effective_name = source_name
            effective["name"] = effective_name
            catalog[catalog.index(by_name[source_name])] = effective
            by_name[source_name] = effective
        else:
            effective_name = unique(source_name if source else "Auto-Push",
                                    (" - " if source else " ") + f"Slot {slot_index + 1}")
            effective["name"] = effective_name
            if not source:
                effective["description"] = "Migrated Auto-Push detector configuration"
            catalog.append(effective)
        seen_sources.add(source_name)
        variants.append((source_name, effective_name, effective))
        assigned.append(effective_name)

    migrated = {"format": "v1simple-profiles", "version": BUNDLE_VERSION,
                "autoPushEnabled": original["autoPushEnabled"], "activeSlot": original["activeSlot"],
                "slots": [{"name": slot["name"], "profile": assigned[index],
                           "color": slot["color"], "alertPersist": slot["alertPersist"],
                           "priorityArrowOnly": slot["priorityArrowOnly"],
                           **SLOT_MODIFIER_DEFAULTS}
                          for index, slot in enumerate(original["slots"])],
                "profiles": catalog}
    validate_bundle(migrated)
    return migrated


def encode_bundle(bundle):
    validate_bundle(bundle)
    raw = json.dumps(bundle, ensure_ascii=False, separators=(",", ":"), allow_nan=False).encode("utf-8")
    require(0 < len(raw) <= MAX_PAYLOAD, "Profile bundle exceeds 128 KiB")
    return raw


def same_bundle(left, right):
    # The filesystem catalog has no stored array order. Slot positions do.
    a, b = migrate_bundle(left), migrate_bundle(right)
    a["profiles"] = sorted(a["profiles"], key=lambda entry: entry["name"])
    b["profiles"] = sorted(b["profiles"], key=lambda entry: entry["name"])
    return a == b


def crc32(raw):
    return f"{zlib.crc32(raw) & 0xffffffff:08x}"


def save_private(path, raw):
    """Create the backup exclusively; never truncate an existing file or symlink."""
    Path(path).parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    try:
        with os.fdopen(descriptor, "wb") as handle:
            os.fchmod(handle.fileno(), 0o600)
            handle.write(raw)
            handle.flush()
            os.fsync(handle.fileno())
    except BaseException:
        Path(path).unlink(missing_ok=True)
        raise


class SerialLink:
    def __init__(self, port=None):
        self.preferred = port
        self.identity = None
        self.serial = None

    def open(self):
        if self.serial is not None:
            return
        try:
            import serial
            from serial.tools import list_ports
        except ImportError as exc:
            raise ProfileError("pyserial is required; use the existing bench Python environment or install pyserial 3.5") from exc
        ports = list(list_ports.comports())
        if self.identity:
            field, value, vid, pid = self.identity
            matches = [item for item in ports if getattr(item, field, None) == value
                       and item.vid == vid and item.pid == pid]
        elif self.preferred:
            matches = [item for item in ports if os.path.realpath(item.device) == os.path.realpath(self.preferred)]
        else:
            matches = [item for item in ports if item.vid == 0x303A]
        if len(matches) > 1:
            raise ProfileError("Multiple matching USB devices; select one with --port")
        if matches:
            item = matches[0]
            path = item.device
            if self.identity is None:
                field = "serial_number" if item.serial_number else "location" if item.location else "device"
                self.identity = (field, getattr(item, field), item.vid, item.pid)
        elif self.preferred and self.identity is None:
            path = self.preferred
        else:
            raise OSError("USB CDC device is not present")
        port = serial.Serial()
        port.port = path
        port.baudrate = 115200
        port.timeout = 0.1
        port.write_timeout = 1
        port.dtr = False
        port.rts = False
        if os.name == "posix":
            port.exclusive = True
        try:
            port.open()
        except BaseException:
            port.close()
            raise
        self.serial = port

    def close(self):
        if self.serial is not None:
            self.serial.close()
            self.serial = None

    def write(self, raw):
        self.open()
        return self.serial.write(raw)

    def read(self, count):
        return self.serial.read(count)


class Client:
    def __init__(self, link, timeout=2.0, attempts=3, clock=time.monotonic, sleep=time.sleep):
        self.link, self.timeout, self.attempts = link, timeout, attempts
        self.clock, self.sleep = clock, sleep
        self.next_id = secrets.randbelow(0xffffffff) + 1
        self.pending = bytearray()
        self.discard_line = False

    def lines(self, chunk):
        for byte in chunk:
            if byte == 10:
                line = bytes(self.pending).rstrip(b"\r") if not self.discard_line else None
                self.pending.clear()
                self.discard_line = False
                if line is not None:
                    yield line
            elif not self.discard_line:
                self.pending.append(byte)
                if len(self.pending) > 255:
                    self.pending.clear()
                    self.discard_line = True

    def command(self, verb, *args, timeout=None, attempts=None):
        request_id = self.next_id
        self.next_id = request_id % 0xffffffff + 1
        # A blank line also terminates any interrupted request from a lost link.
        wire = (f"\n@V1USB1 {request_id} {verb}" + "".join(f" {arg}" for arg in args) + "\n").encode("ascii")
        require(len(wire) <= 256, "USB request exceeds transport limit")
        limit = self.timeout if timeout is None else timeout
        for attempt in range(self.attempts if attempts is None else attempts):
            deadline = self.clock() + limit
            try:
                offset = 0
                while offset < len(wire) and self.clock() < deadline:
                    written = self.link.write(wire[offset:])
                    if not written:
                        raise OSError("USB write stalled")
                    offset += written
                if offset != len(wire):
                    raise OSError("USB write incomplete")
                while self.clock() < deadline:
                    for line in self.lines(self.link.read(256)):
                        match = re.fullmatch(rb"@V1USB1 ([0-9]{1,10}) (\{.*\})", line)
                        if not match or int(match[1]) != request_id:
                            continue
                        try:
                            reply = parse_json(match[2])
                        except ProfileError:
                            continue  # Interleaved logs can damage a reply; exact retry is cached.
                        if type(reply) is not dict or type(reply.get("ok")) is not bool:
                            continue
                        if not reply["ok"]:
                            code = reply.get("error")
                            code = code if type(code) is str and re.fullmatch(r"[a-z0-9_]{1,64}", code) else "rejected"
                            raise DeviceError(f"Device rejected the command: {code}")
                        return reply
            except OSError:
                self.link.close()
                self.pending.clear()
                self.discard_line = False
                self.sleep(min(0.15, limit))
        raise TransportError(f"No complete USB acknowledgement for {verb}; operation not confirmed")


class Device:
    def __init__(self, client, mode_timeout=45):
        self.client, self.mode_timeout = client, mode_timeout

    def status(self, **kwargs):
        value = self.client.command("status", **kwargs)
        require(value.get("mode") in ("normal", "maintenance") and integer(value.get("boot"), 0, 0xffffffff),
                "Device returned invalid mode/boot status")
        require(all(type(value.get(key)) is str and re.fullmatch(r"[0-9a-fA-F]{7,64}", value[key])
                    for key in ("git", "image")), "Device returned invalid firmware identity")
        require(integer(value.get("slot"), 0, 2) and integer(value.get("persist"), 0, 5)
                and type(value.get("enabled")) is bool and type(value.get("busy")) is bool,
                "Device returned invalid profile status")
        return {key: value[key] for key in ("mode", "boot", "git", "image", "slot", "persist", "enabled", "busy")}

    def mode(self, target, original=None):
        original = self.status() if original is None else original
        if original["mode"] == target:
            return original
        try:
            self.client.command(target)
        except TransportError:
            pass  # A controlled restart may remove the ACK; status is authoritative.
        deadline = self.client.clock() + self.mode_timeout
        while self.client.clock() < deadline:
            try:
                current = self.status(timeout=min(2, deadline - self.client.clock()), attempts=1)
                require(all(current[key] == original[key] for key in ("git", "image")),
                        "Firmware identity changed during mode transition")
                if current["mode"] == target and current["boot"] != original["boot"]:
                    return current
            except TransportError:
                pass
            self.client.sleep(0.1)
        raise TransportError("Requested mode and a new boot were not confirmed")

    @contextmanager
    def maintenance(self, stay=False):
        original = self.status()
        self.mode("maintenance", original)
        error = None
        try:
            yield original
        except BaseException as exc:
            error = exc
            raise
        finally:
            if not stay and original["mode"] == "normal":
                try:
                    self.mode("normal")
                except ProfileError as exc:
                    message = "Could not confirm return to normal mode"
                    if error is not None:
                        message = "Operation failed, and return to normal mode could not be confirmed"
                    raise TransportError(message) from exc

    def backup(self):
        result = self.client.command("backup", timeout=30)
        length, checksum = result.get("bytes"), result.get("crc32")
        require(integer(length, 1, MAX_PAYLOAD) and type(checksum) is str
                and re.fullmatch(r"[0-9a-fA-F]{8}", checksum), "Invalid backup length/checksum")
        raw = bytearray()
        while len(raw) < length:
            reply = self.client.command("read", len(raw))
            encoded = reply.get("data")
            require(type(reply.get("offset")) is int and reply["offset"] == len(raw)
                    and type(encoded) is str and re.fullmatch(r"(?:[0-9a-fA-F]{2}){1,64}", encoded),
                    "Invalid backup chunk")
            chunk = bytes.fromhex(encoded)
            require(len(raw) + len(chunk) <= length, "Backup chunk exceeds declared length")
            raw.extend(chunk)
        require(crc32(raw) == checksum.lower(), "Backup CRC32 mismatch; no backup or success was recorded")
        bundle = validate_bundle(parse_json(raw))
        return bytes(raw), bundle

    def replace(self, bundle):
        # Normalize legacy input before entering a device transaction. A v1
        # catalog can grow by deterministic per-slot variants; reject an
        # over-cap expansion before maintenance or any upload begins.
        bundle = migrate_bundle(bundle)
        raw = encode_bundle(bundle)
        started = committing = False
        try:
            reply = self.client.command("begin", len(raw), crc32(raw))
            started = True
            require(type(reply.get("offset")) is int and reply["offset"] == 0, "Invalid upload start offset")
            for offset in range(0, len(raw), CHUNK):
                chunk = raw[offset:offset + CHUNK]
                reply = self.client.command("write", offset, chunk.hex())
                require(type(reply.get("offset")) is int and reply["offset"] == offset + len(chunk),
                        "Upload acknowledgement has an incorrect offset")
            committing = True
            reply = self.client.command("commit", timeout=30)
            require(reply.get("stored") is True and type(reply.get("backup_pending")) is bool
                    and reply.get("migration_pending") is False
                    and type(reply.get("profiles")) is int and reply["profiles"] == len(bundle["profiles"]),
                    "Profile storage commit or detector-profile migration was not confirmed")
            _, observed = self.backup()
            require(same_bundle(observed, bundle), "Stored profile bundle does not match the requested replacement")
            return {"stored": True, "readback_verified": True, "backup_pending": reply["backup_pending"]}
        except BaseException:
            if started and not committing:
                try:
                    self.client.command("abort", attempts=1)
                except ProfileError:
                    pass
            raise


def backup_path():
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    return Path(__file__).resolve().parents[1] / ".artifacts/usb-profiles" / f"profiles-before-{stamp}.json"


def perform(args, device, announce=print):
    if args.command == "status":
        return device.status()
    if args.command in ("maintenance", "normal"):
        return device.mode(args.command)
    requested = None
    if args.command in ("load", "restore"):
        with args.file.open("rb") as handle:
            raw_input = handle.read(MAX_PAYLOAD + 1)
        require(len(raw_input) <= MAX_PAYLOAD, "Profile bundle exceeds 128 KiB")
        requested = migrate_bundle(validate_bundle(parse_json(raw_input)))
        encode_bundle(requested)  # Validate/migrate before entering maintenance or touching storage.
    with device.maintenance(args.stay_maintenance) as original:
        raw, current = device.backup()
        if args.command == "backup":
            save_private(args.file, raw)
            announce(f"Backup saved: {args.file}")
            result = {"backup_verified": True, "bytes": len(raw)}
        else:
            before = args.backup_before or backup_path()
            save_private(before, raw)
            announce(f"Backup before replacement: {before}")
            if args.command == "set-slot":
                requested = deepcopy(current)
                requested["slots"][args.slot]["alertPersist"] = args.persistence
            result = device.replace(requested)
    if requested is not None:
        result["normal_consumer_verified"] = False
        if original["mode"] == "normal" and not args.stay_maintenance:
            status = device.status()
            active = requested["activeSlot"]
            require(status["mode"] == "normal" and status["slot"] == active
                    and status["persist"] == requested["slots"][active]["alertPersist"]
                    and status["enabled"] == requested["autoPushEnabled"],
                    "Normal firmware did not report the requested active slot settings after reboot")
            result["normal_consumer_verified"] = True
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", help="USB CDC port; required if multiple devices match")
    parser.add_argument("--stay-maintenance", action="store_true", help="keep maintenance mode after bulk operations")
    commands = parser.add_subparsers(dest="command", required=True)
    for name in ("status", "maintenance", "normal"):
        commands.add_parser(name)
    commands.add_parser("backup").add_argument("file", type=Path)
    for name in ("load", "restore"):
        sub = commands.add_parser(name)
        sub.add_argument("file", type=Path)
        sub.add_argument("--backup-before", type=Path)
    sub = commands.add_parser("set-slot")
    sub.add_argument("slot", type=int, choices=range(3))
    sub.add_argument("--persistence", type=int, choices=range(6), required=True)
    sub.add_argument("--backup-before", type=Path)
    args = parser.parse_args(argv)
    link = SerialLink(args.port)
    try:
        result = perform(args, Device(Client(link)), announce=lambda line: print(line, flush=True))
        print(json.dumps(result, sort_keys=True))
        return 0
    except ProfileError as exc:
        print(f"Profile operation failed: {exc}", file=sys.stderr)
        return 1
    except OSError as exc:
        print(f"Profile file or USB operation failed: {exc.strerror or 'I/O unavailable'}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("Profile operation interrupted; no success is claimed", file=sys.stderr)
        return 130
    finally:
        link.close()


if __name__ == "__main__":
    raise SystemExit(main())
