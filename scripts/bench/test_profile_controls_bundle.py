#!/usr/bin/env python3
"""Host-only fixture preparation and preservation checks."""
from copy import deepcopy
import json
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
import unittest

import profile_controls_bundle as controls

usb = controls.usb


def original_bundle():
    box = usb.default_in_the_box()
    box["bands"]["ka"]["muteOutside"] = True
    return {"format": "v1simple-profiles", "version": usb.BUNDLE_VERSION,
            "autoPushEnabled": False, "activeSlot": 2,
            "profiles": [{"schemaVersion": 4, "name": "Original", "description": "Keep this",
                          "rawBytes": [129, 255, 165, 90, 213, 44],
                          "detector": usb.default_detector("unchanged"), "inTheBox": box}],
            "slots": [{"name": name, "profile": "Original", "color": index + 100,
                       "alertPersist": index + 1, "priorityArrowOnly": True,
                       "volumeOverride": True, "volume": 6, "muteVolume": 2,
                       "darkModeOverride": True, "darkMode": True}
                      for index, name in enumerate(("DEFAULT", "HIGHWAY", "CITY"))]}


class ProfileControlsBundleTests(unittest.TestCase):
    def test_exact_fixture_and_preserved_original_catalog_and_slot_identity(self):
        source = original_bundle()
        before = deepcopy(source)
        baseline = [0x81, 0xFF, 0xA5, 0x5A, 0xD5, 0x2C]
        result = controls.prepare_bundle(source, baseline)
        self.assertEqual(source, before)
        self.assertEqual(baseline, [0x81, 0xFF, 0xA5, 0x5A, 0xD5, 0x2C])
        self.assertEqual(result["profiles"][:-1], before["profiles"])
        self.assertEqual(result["activeSlot"], before["activeSlot"])
        self.assertTrue(result["autoPushEnabled"])
        for actual, original in zip(result["slots"], before["slots"]):
            self.assertEqual(actual, {**original, **usb.SLOT_MODIFIER_DEFAULTS,
                                     "profile": controls.PROFILE_NAME,
                                     "alertPersist": 0, "priorityArrowOnly": False})
        profile = result["profiles"][-1]
        self.assertEqual(profile["rawBytes"], [0x8F, 0xF7, 0xA5, 0x5A, 0xD5, 0x2C])
        self.assertEqual(profile["detector"], {
            **usb.default_detector(), "customFrequencies": {"policy": "value", "definitions": [
                {"index": 0, "lowerMHz": 24000, "upperMHz": 24200},
                {"index": 1, "lowerMHz": 33900, "upperMHz": 35000}]}})
        expected_box = usb.default_in_the_box()
        expected_box["bands"]["k"] = {"muteOutside": True, "unmuteInside": True}
        expected_box["boxes"]["k"]["upperMHz"] = 24150
        self.assertEqual(profile["inTheBox"], expected_box)
        self.assertEqual(usb.parse_json(usb.encode_bundle(result)), result)

    def test_legacy_backup_uses_shipping_migration(self):
        source = original_bundle()
        source["version"] = 3
        for profile in source["profiles"]:
            profile["schemaVersion"] = 3
            del profile["inTheBox"]
        for slot in source["slots"]:
            for key in usb.SLOT_MODIFIER_DEFAULTS:
                del slot[key]
        result = controls.prepare_bundle(source, [255] * 6)
        self.assertEqual(result["profiles"][:-1], usb.migrate_bundle(source)["profiles"])
        self.assertEqual(result["version"], usb.BUNDLE_VERSION)

    def test_inactive_box_cli_restores_exact_defaults_and_keeps_scan_fixture(self):
        source_bundle = original_bundle()
        active = controls.prepare_bundle(source_bundle, [127, 255, 255, 255, 255, 255])
        with tempfile.TemporaryDirectory() as directory:
            source, output = Path(directory) / "original.json", Path(directory) / "inactive.json"
            original = usb.encode_bundle(source_bundle)
            source.write_bytes(original)
            completed = subprocess.run([
                sys.executable, str(Path(controls.__file__)), str(source), str(output),
                "--baseline-user-bytes", "7fffffffffff", "--inactive-box-policy"],
                capture_output=True, text=True)
            self.assertEqual(completed.returncode, 0, completed.stderr)
            inactive = usb.parse_json(output.read_bytes())
            self.assertEqual(inactive["profiles"][-1]["inTheBox"], usb.default_in_the_box())
            self.assertEqual(inactive["profiles"][-1]["detector"]["customFrequencies"],
                             active["profiles"][-1]["detector"]["customFrequencies"])
            active["profiles"][-1]["inTheBox"] = usb.default_in_the_box()
            active["profiles"][-1]["description"] = inactive["profiles"][-1]["description"]
            self.assertEqual(inactive, active)
            self.assertEqual(source.read_bytes(), original)

    def test_reserved_name_and_full_catalog_rejected_without_replacement(self):
        source = original_bundle()
        source["profiles"][0]["name"] = controls.PROFILE_NAME.swapcase()
        for slot in source["slots"]:
            slot["profile"] = source["profiles"][0]["name"]
        with self.assertRaisesRegex(usb.ProfileError, "already exists"):
            controls.prepare_bundle(source, [255] * 6)
        source = original_bundle()
        for index in range(9):
            profile = deepcopy(source["profiles"][0])
            profile["name"] = f"Spare {index}"
            source["profiles"].append(profile)
        with self.assertRaisesRegex(usb.ProfileError, "one free slot"):
            controls.prepare_bundle(source, [255] * 6)

    def test_invalid_backup_and_baseline_are_rejected(self):
        for raw in ("", "FFFFFFFFFF", "FFFFFFFFFFFF00", "GGFFFFFFFFFF", "FF FF FF FF FF FF"):
            with self.subTest(raw=raw), self.assertRaises(usb.ProfileError):
                controls.parse_user_bytes(raw)
        self.assertEqual(controls.parse_user_bytes("7fFFFFFFFFFF"), [127, 255, 255, 255, 255, 255])
        for raw in ([255] * 5, [255] * 7, [True] * 6, [256] * 6):
            with self.subTest(raw=raw), self.assertRaises(usb.ProfileError):
                controls.prepare_bundle(original_bundle(), raw)
        source = original_bundle()
        source["unexpected"] = True
        with self.assertRaises(usb.ProfileError):
            controls.prepare_bundle(source, [255] * 6)

    def test_cli_creates_private_output_without_touching_input_or_existing_paths(self):
        with tempfile.TemporaryDirectory() as directory:
            source, output = Path(directory) / "original.json", Path(directory) / "fixture.json"
            original = json.dumps(original_bundle(), indent=2).encode()
            source.write_bytes(original)
            completed = subprocess.run([
                sys.executable, str(Path(controls.__file__)), str(source), str(output),
                "--baseline-user-bytes", "7FFFFFFFFFFF"], capture_output=True, text=True)
            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertEqual(source.read_bytes(), original)
            self.assertEqual(stat.S_IMODE(output.stat().st_mode), 0o600)
            self.assertEqual(usb.parse_json(output.read_bytes())["profiles"][-1]["rawBytes"],
                             [0x7F, 0xF7, 0xFF, 0xFF, 0xFF, 0xFF])
            with self.assertRaises(FileExistsError):
                controls.prepare_file(source, output, [255] * 6)
            with self.assertRaisesRegex(usb.ProfileError, "differ"):
                controls.prepare_file(source, source, [255] * 6)
            alias = Path(directory) / "alias.json"
            alias.symlink_to(source)
            with self.assertRaisesRegex(usb.ProfileError, "differ"):
                controls.prepare_file(source, alias, [255] * 6)
            self.assertEqual(source.read_bytes(), original)


if __name__ == "__main__":
    unittest.main()
