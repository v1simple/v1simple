#!/usr/bin/env python3
"""Host protocol adversity and exact profile-preservation regressions."""
from copy import deepcopy
import json
import os
from pathlib import Path
from types import SimpleNamespace
import sys
import tempfile
import unittest
from unittest.mock import patch

import usb_profiles as usb


def bundle():
    return {"format": "v1simple-profiles", "version": 1, "autoPushEnabled": True,
        "activeSlot": 1, "slots": [
            {"name": name, "profile": "Road", "mode": index, "color": 100 + index,
             "volumeConfigured": index != 0, "volume": index, "muteVolume": 0,
             "darkMode": index == 2, "muteToZero": True, "alertPersist": index,
             "priorityArrowOnly": index == 1}
            for index, name in enumerate(("DEFAULT", "HIGHWAY", "CITY"))],
        "profiles": [{"name": name, "description": "A private fixture description",
            "rawBytes": [1, 17, 33, 65, 129, 255], "displayOn": True,
            "mainVolume": 0, "mutedVolume": 255} for name in ("Road", "Spare")]}


class Clock:
    def __init__(self):
        self.now = 0.0

    def __call__(self):
        return self.now

    def sleep(self, duration):
        self.now += duration


class WireDevice:
    """Byte-level peer with an independent catalog and interrupted replies."""
    def __init__(self, clock):
        self.clock = clock
        self.current = bundle()
        self.mode = "normal"
        self.boot = 10
        self.input = bytearray()
        self.output = bytearray()
        self.history = []
        self.cached = None
        self.commits = 0
        self.drop_once = set()
        self.drop_always = set()
        self.torn_once = set()
        self.bad_crc = False
        self.no_transition = False
        self.extra_after_commit = False
        self.wrong_normal_consumer = False
        self.read_count = 0
        self.upload = bytearray()

    def close(self):
        pass

    def read(self, count):
        self.read_count += 1
        size = min(count, (1, 7, 3, 31, 11)[self.read_count % 5])
        self.clock.now += .001 if self.output else .01
        result = bytes(self.output[:size])
        del self.output[:size]
        return result

    def write(self, data):
        # Exercise partial host writes independently of fragmented replies.
        consumed = min(23, len(data))
        self.input.extend(data[:consumed])
        self.clock.now += .0001
        while b"\n" in self.input:
            line, _, rest = self.input.partition(b"\n")
            self.input = bytearray(rest)
            if not line:
                continue
            parts = line.decode("ascii").split()
            assert parts[0] == "@V1USB1"
            request_id, verb, args = int(parts[1]), parts[2], parts[3:]
            self.history.append((request_id, verb, args))
            if self.cached and self.cached[0] == request_id:
                assert self.cached[1] == bytes(line), "retry changed its request"
                reply = self.cached[2]
            else:
                reply = self.dispatch(verb, args)
                self.cached = (request_id, bytes(line), reply)
            if verb in self.drop_always:
                continue
            if verb in self.drop_once:
                self.drop_once.remove(verb)
                continue
            frame = b"\n@V1USB1 " + str(request_id).encode() + b" " + json.dumps(reply, separators=(",", ":")).encode() + b"\n"
            if verb in self.torn_once:
                self.torn_once.remove(verb)
                frame = frame[:35] + b"ordinary log interleaved\n"
            self.output.extend(b"unrelated partial log\n" + frame)
        return consumed

    def dispatch(self, verb, args):
        if verb == "status":
            persist = self.current["slots"][self.current["activeSlot"]]["alertPersist"]
            return {"ok": True, "mode": self.mode, "boot": self.boot,
                "git": "1234567", "image": "abcdef123", "slot": self.current["activeSlot"],
                "persist": (persist + 1) % 6 if self.wrong_normal_consumer and self.commits and self.mode == "normal" else persist,
                "enabled": self.current["autoPushEnabled"], "busy": False}
        if verb in ("maintenance", "normal"):
            if self.mode == verb:
                return {"ok": True, "mode": verb}
            if not self.no_transition:
                self.mode = verb
                self.boot += 1
            return {"ok": True, "mode": "restarting"}
        assert self.mode == "maintenance"
        if verb == "backup":
            exported = deepcopy(self.current)
            exported["profiles"].reverse()  # Catalog order is not storage meaning.
            self.download = json.dumps(exported, separators=(",", ":")).encode()
            return {"ok": True, "bytes": len(self.download),
                    "crc32": "00000000" if self.bad_crc else usb.crc32(self.download).upper()}
        if verb == "read":
            offset = int(args[0])
            return {"ok": True, "offset": offset, "data": self.download[offset:offset + 64].hex()}
        if verb == "begin":
            self.upload = bytearray()
            self.length, self.checksum = int(args[0]), args[1]
            return {"ok": True, "offset": 0}
        if verb == "write":
            assert int(args[0]) == len(self.upload)
            self.upload.extend(bytes.fromhex(args[1]))
            return {"ok": True, "offset": len(self.upload)}
        if verb == "commit":
            assert len(self.upload) == self.length and usb.crc32(self.upload) == self.checksum
            self.current = json.loads(self.upload)
            self.commits += 1
            count = len(self.current["profiles"])
            if self.extra_after_commit:
                extra = deepcopy(self.current["profiles"][0])
                extra["name"] = "Unexpected"
                self.current["profiles"].append(extra)
            return {"ok": True, "stored": True, "backup_pending": True, "profiles": count}
        if verb == "abort":
            self.upload.clear()
            return {"ok": True}
        raise AssertionError(verb)


class USBProfilesTests(unittest.TestCase):
    def setUp(self):
        self.clock = Clock()
        self.peer = WireDevice(self.clock)
        self.client = usb.Client(self.peer, timeout=.5, attempts=3, clock=self.clock, sleep=self.clock.sleep)
        self.device = usb.Device(self.client, mode_timeout=.3)
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)

    def args(self, command, **kwargs):
        return SimpleNamespace(command=command, stay_maintenance=False, backup_before=None, **kwargs)

    def test_fragmented_torn_log_interleaved_reply_retries_exact_request(self):
        self.peer.torn_once.add("status")
        self.peer.output.extend(b"x" * 600 + b"\n@V1USB1 0 {\"ok\":true}\n")
        status = self.device.status()
        self.assertEqual(status["mode"], "normal")
        self.assertEqual(len(self.peer.history), 2)
        self.assertEqual(self.peer.history[0], self.peer.history[1])

    def test_crc_failure_never_creates_backup_and_returns_normal(self):
        self.peer.bad_crc = True
        destination = self.directory / "backup.json"
        with self.assertRaisesRegex(usb.ProfileError, "CRC32"):
            usb.perform(self.args("backup", file=destination), self.device)
        self.assertFalse(destination.exists())
        self.assertEqual(self.peer.mode, "normal")
        self.assertEqual(self.peer.commits, 0)

    def test_lost_mode_ack_still_requires_observed_mode_and_new_boot(self):
        self.peer.drop_once.add("maintenance")
        status = self.device.mode("maintenance")
        self.assertEqual((status["mode"], status["boot"]), ("maintenance", 11))
        self.peer.no_transition = True
        with self.assertRaisesRegex(usb.TransportError, "new boot"):
            self.device.mode("normal")

    def test_timeout_has_no_false_success_and_bounded_retries(self):
        self.peer.drop_always.add("status")
        with self.assertRaises(usb.TransportError):
            self.device.status()
        self.assertEqual(len(self.peer.history), 3)
        self.assertEqual(len({row[0] for row in self.peer.history}), 1)

    def test_set_slot_changes_only_requested_persistence_and_saves_original(self):
        before = deepcopy(self.peer.current)
        saved = self.directory / "before.json"
        args = self.args("set-slot", slot=1, persistence=2)
        args.backup_before = saved
        messages = []
        result = usb.perform(args, self.device, announce=messages.append)
        expected = deepcopy(before)
        expected["slots"][1]["alertPersist"] = 2
        self.assertTrue(usb.same_bundle(self.peer.current, expected))
        self.assertTrue(usb.same_bundle(json.loads(saved.read_bytes()), before))
        self.assertEqual(saved.stat().st_mode & 0o777, 0o600)
        self.assertEqual(self.peer.mode, "normal")
        self.assertEqual(result, {"stored": True, "readback_verified": True, "backup_pending": True,
                                  "normal_consumer_verified": True})
        self.assertFalse(any("private fixture" in message or "rawBytes" in message for message in messages))

    def test_restore_replaces_complete_catalog_and_preserves_all_values(self):
        original = bundle()
        original["profiles"].append({**deepcopy(original["profiles"][0]), "name": "OnlyBackup", "displayOn": False})
        original["slots"][0]["profile"] = "OnlyBackup"
        original["slots"][2]["volume"] = 0
        original["activeSlot"] = 2
        file = self.directory / "restore.json"
        file.write_bytes(usb.encode_bundle(original))
        self.peer.current["profiles"].append({**deepcopy(original["profiles"][0]), "name": "DeleteMe"})
        args = self.args("restore", file=file)
        args.backup_before = self.directory / "before-restore.json"
        result = usb.perform(args, self.device, announce=lambda _: None)
        self.assertTrue(result["readback_verified"])
        self.assertTrue(usb.same_bundle(self.peer.current, original))
        self.assertNotIn("DeleteMe", [entry["name"] for entry in self.peer.current["profiles"]])

    def test_extra_readback_profile_is_not_ignored(self):
        self.peer.extra_after_commit = True
        with self.device.maintenance():
            with self.assertRaisesRegex(usb.ProfileError, "does not match"):
                self.device.replace(bundle())

    def test_stored_readback_does_not_hide_wrong_normal_consumer(self):
        self.peer.wrong_normal_consumer = True
        args = self.args("set-slot", slot=1, persistence=3)
        args.backup_before = self.directory / "before.json"
        with self.assertRaisesRegex(usb.ProfileError, "Normal firmware"):
            usb.perform(args, self.device, announce=lambda _: None)
        self.assertEqual(self.peer.current["slots"][1]["alertPersist"], 3)
        self.assertEqual(self.peer.mode, "normal")

    def test_stay_maintenance_and_original_maintenance_do_not_claim_normal_consumption(self):
        for start, stay in (("normal", True), ("maintenance", False)):
            self.peer.mode = start
            args = self.args("set-slot", slot=1, persistence=3)
            args.stay_maintenance = stay
            args.backup_before = self.directory / f"before-{start}.json"
            result = usb.perform(args, self.device, announce=lambda _: None)
            self.assertTrue(result["readback_verified"])
            self.assertFalse(result["normal_consumer_verified"])
            self.assertEqual(self.peer.mode, "maintenance")

    def test_lost_commit_ack_is_cached_once_and_missing_ack_is_not_success(self):
        self.peer.mode = "maintenance"
        self.peer.drop_once.add("commit")
        self.assertTrue(self.device.replace(bundle())["readback_verified"])
        self.assertEqual(self.peer.commits, 1)
        self.peer.drop_always.add("commit")
        with self.assertRaises(usb.TransportError):
            self.device.replace(bundle())
        self.assertEqual(self.peer.commits, 2)

    def test_backup_and_before_backup_never_overwrite_existing_file_or_symlink(self):
        existing = self.directory / "existing.json"
        existing.write_bytes(b"preserve this file")
        link = self.directory / "link.json"
        link.symlink_to(existing)
        for path in (existing, link):
            with self.assertRaises(FileExistsError):
                usb.save_private(path, b"replacement")
            self.assertEqual(existing.read_bytes(), b"preserve this file")
        args = self.args("set-slot", slot=0, persistence=3)
        args.backup_before = existing
        with self.assertRaises(FileExistsError):
            usb.perform(args, self.device)
        self.assertEqual(self.peer.commits, 0)

    def test_rejects_duplicate_unknown_fields_and_wrong_scalar_types(self):
        with self.assertRaises(usb.ProfileError):
            usb.parse_json(b'{"version":1,"version":1}')
        for change in (lambda d: d.update(extra=0), lambda d: d.update(activeSlot=True),
                       lambda d: d["slots"][0].update(alertPersist=6),
                       lambda d: d["profiles"][0].update(rawBytes=[True] * 6),
                       lambda d: d["profiles"][0].update(name="../escape"),
                       lambda d: d["profiles"][0].update(description="\ud800")):
            document = bundle()
            change(document)
            with self.assertRaises(usb.ProfileError):
                usb.encode_bundle(document)

    def test_serial_controls_are_false_before_open_and_reenumeration_keeps_identity(self):
        events = []
        class Port:
            def open(self):
                events.append((self.port, self.dtr, self.rts))
            def close(self):
                pass
        first = SimpleNamespace(device="/fixture/one", vid=0x303A, pid=0x1001,
                                serial_number="fixture-serial", location="fixture-location")
        current = [first]
        ports = SimpleNamespace(comports=lambda: current)
        with patch.dict(sys.modules, {"serial": SimpleNamespace(Serial=Port),
                                      "serial.tools": SimpleNamespace(list_ports=ports)}):
            link = usb.SerialLink()
            link.open()
            link.close()
            current[:] = [SimpleNamespace(**{**vars(first), "device": "/fixture/two"})]
            link.open()
            link.close()
        self.assertEqual(events, [("/fixture/one", False, False), ("/fixture/two", False, False)])


if __name__ == "__main__":
    unittest.main()
