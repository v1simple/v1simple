"""Host-only recovery ownership, durable intent and semantic preservation checks."""
from copy import deepcopy
import io
import json
import os
from pathlib import Path
import stat
import sys
import tempfile
import unittest
from unittest.mock import patch
import urllib.error
from urllib.parse import parse_qs, urlsplit

sys.path.insert(0, str(Path(__file__).resolve().parent / "bench"))
import behavior_state as state
from behavior_browser import COLOR_CONTROLS, TOGGLES


def snapshot():
    display = {key: index + 1 for index, key in enumerate(COLOR_CONTROLS)}
    display.update({key: False for key in TOGGLES})
    display["brightness"] = 200
    return {
        "backup": {"_type": "v1simple_backup", "_version": 1, "_timestamp": 10,
                   "timestamp": 10, "profiles": [{"name": "Zulu"}, {"name": "Alpha"}],
                   "brightness": 200, "unrelatedSetting": 7},
        "display": display,
        "quiet": {"stealthEnabled": False}, "audio": {"enabled": False},
        "obd": {"enabled": False}, "gps": {"enabled": False},
        "slots": {"schemaVersion": 4, "detectorConfigurationOwner": "profile",
                  "activeSlot": 0, "enabled": True, "slots": [
                      {"name": name, "profile": "", "color": index + 1,
                       "volumeConfigured": False, "volume": 0, "muteVolume": 0,
                       "darkModeConfigured": False, "darkMode": False,
                       "alertPersist": index, "priorityArrowOnly": False}
                      for index, name in enumerate(("DEFAULT", "HIGHWAY", "CITY"))]},
        "device": {"ap_ssid": "Fixture", "ap_password": "********", "proxy_name": "Fixture",
                   "isDefaultPassword": False, "proxy_ble": True, "alpEnabled": False,
                   "alpDisableV1LaserOnPush": False, "autoPowerOffMinutes": 0,
                   "apTimeoutMinutes": 0, "alpAlertPersistSec": 0,
                   "nvsDiag": {"ns": "settings-a", "valid": 1, "ver": 1, "bright": 200,
                               "proxy": True, "autoPush": True, "healthy": True}},
        "devices": {"count": 2, "devices": [
            {"address": "AA:BB:CC:00:00:02", "name": "B", "defaultProfile": 3, "connected": False},
            {"address": "AA:BB:CC:00:00:01", "name": "A", "defaultProfile": 0, "connected": False}]},
    }


def profiles():
    return {"format": "v1simple-profiles", "version": 5, "autoPushEnabled": True,
            "activeSlot": 0, "profiles": [], "slots": [
                {"name": name, "profile": "", "color": index + 1, "alertPersist": index,
                 "priorityArrowOnly": False, "volumeOverride": False, "volume": 255,
                 "muteVolume": 255, "darkModeOverride": False, "darkMode": False}
                for index, name in enumerate(("DEFAULT", "HIGHWAY", "CITY"))]}


IDENTITY = {"git": "123abcd", "image": "a" * 64, "mode": "normal"}
USB_IDENTITY = ["serial_number", "fixture", 0x303A, 0x1001]


class JournalTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.journal = state.RecoveryJournal.create(
            self.directory, IDENTITY, snapshot(), profiles(), USB_IDENTITY)

    def reload(self):
        return state.RecoveryJournal.load(self.directory)

    def test_audio_intent_restores_only_owned_values_and_rejects_intervening_edits(self):
        baseline = snapshot()
        baseline["audio"] = {"voiceVolume": 31, "muteVoiceIfVolZero": False, "voiceAlertMode": 3}
        directory = self.directory / "audio"
        directory.mkdir()
        journal = state.RecoveryJournal.create(directory, IDENTITY, baseline, profiles(), USB_IDENTITY)
        journal.audio_intent(baseline["audio"], {"voiceVolume": 97})
        recovered = state.RecoveryJournal.load(directory)
        current = {**baseline["audio"], "voiceVolume": 97, "muteVoiceIfVolZero": True}
        self.assertEqual(recovered.audio_restore_patch(current), {"voiceVolume": 31})
        with self.assertRaises(state.Incomplete):
            recovered.audio_intent(current, {"muteVoiceIfVolZero": False})
        with self.assertRaises(state.Incomplete):
            recovered.audio_restore_patch({**current, "voiceVolume": 55})
        with self.assertRaises(state.Incomplete):
            recovered.audio_intent(current, {"voiceVolume": 101})

    def test_autopush_enable_recovery_keeps_slot_identity_and_separate_persistence(self):
        slots = snapshot()["slots"]
        self.journal.autopush_intent(slots, False)
        changed = deepcopy(slots)
        changed["enabled"] = False
        self.journal.validate_persistence_change(changed, 0)
        self.journal.persistence_intent(0, 3)
        changed["slots"][0]["alertPersist"] = 3
        self.assertEqual(self.reload().autopush_restore(changed), {"slot": 0, "enable": True})
        self.assertEqual(self.reload().persistence_restore(changed), [(0, 0)])
        changed["activeSlot"] = 1
        with self.assertRaises(state.Incomplete):
            self.reload().autopush_restore(changed)

    def test_replay_metadata_requires_fresh_capture_and_keeps_other_differences(self):
        capture = {"available": True, "address": "AA:BB:CC:DD:EE:FF", "provenance": {
            "capturedBootId": 8, "captureTimedOut": False, "sessionGeneration": 1}}
        with self.assertRaises(state.Incomplete):
            self.journal.admit_replay_target(capture, 7)
        self.journal.admit_replay_target(capture, 8)
        changed = snapshot()
        changed["backup"]["lastV1Address"] = capture["address"]
        drift, metadata = self.reload().restoration_differences(changed)
        self.assertEqual(drift, [])
        self.assertEqual(metadata["observed"], capture["address"])
        changed["backup"]["unrelatedSetting"] = 9
        self.assertEqual(self.reload().restoration_differences(changed)[0], ["backup.unrelatedSetting"])
        with self.assertRaises(state.Incomplete):
            self.journal.admit_replay_target({**capture, "address": "AA:BB:CC:DD:EE:00"}, 8)

    def test_profile_ownership_recovery_requires_exact_document_and_no_references(self):
        import usb_profiles
        baseline = snapshot()
        baseline["backup"]["profiles"] = []
        directory = self.directory / "profile"
        directory.mkdir()
        journal = state.RecoveryJournal.create(directory, IDENTITY, baseline, profiles(), USB_IDENTITY,
                                               profile_documents={})
        initial = {"schemaVersion": 4, "name": "bench-fixture", "description": "created",
                   "settings": {"bytes": [255] * 6}, "detector": usb_profiles.default_detector(),
                   "inTheBox": usb_profiles.default_in_the_box()}
        edited = {**initial, "description": "edited"}
        journal.profile_intent("create", None, initial)
        journal.profile_intent("edit", initial, edited)
        recovered = state.RecoveryJournal.load(directory)
        self.assertEqual(recovered.owned_profile({initial["name"]: initial}, baseline["slots"]), initial["name"])
        self.assertEqual(recovered.owned_profile({initial["name"]: edited}, baseline["slots"]), initial["name"])
        with self.assertRaises(state.Incomplete):
            recovered.owned_profile({initial["name"]: {**edited, "description": "someone else"}}, baseline["slots"])
        slots = deepcopy(baseline["slots"])
        slots["slots"][0]["profile"] = initial["name"]
        with self.assertRaises(state.Incomplete):
            recovered.owned_profile({initial["name"]: edited}, slots)
        # A host loss before the edit committed leaves the initial owned document.
        recovered.profile_intent("delete", initial, None)
        self.assertEqual(state.RecoveryJournal.load(directory).value["profile_intents"][-1]["before"], initial)

    def test_baseline_is_private_durable_and_not_aliased(self):
        self.assertEqual(stat.S_IMODE(self.journal.path.stat().st_mode), 0o600)
        self.assertEqual(self.reload().value, self.journal.value)
        self.journal.value["identity"]["git"] = "7654321"
        self.assertEqual(IDENTITY["git"], "123abcd")
        self.assertEqual(self.reload().value["identity"]["git"], "123abcd")

    def test_killed_writer_temporary_file_cannot_block_new_intent(self):
        stale = self.directory / "recovery.json.tmp"
        stale.write_text("interrupted writer")
        self.journal.display_intent({"freq": 0xF800})
        self.assertEqual(self.reload().value["display_intents"], [{"freq": 0xF800}])
        self.assertEqual(stale.read_text(), "interrupted writer")

    def test_failed_durable_intent_cannot_authorize_in_memory_restoration(self):
        before = deepcopy(self.journal.value)
        with patch.object(state.os, "replace", side_effect=OSError("fixture disk failure")):
            with self.assertRaises(OSError):
                self.journal.display_intent({"freq": 0xF800})
        self.assertEqual(self.journal.value, before)
        self.assertEqual(self.reload().value, before)
        self.assertEqual(list(self.directory.glob("*.tmp")), [])

    def test_display_recovery_owns_only_durably_recorded_fields(self):
        original = snapshot()["display"]
        self.journal.display_intent({"freq": 0xF800})
        recovered = self.reload()
        current = {**original, "freq": 0xF800, "bogey": 0x07E0}
        self.assertEqual(recovered.display_restore_patch(current), {"freq": original["freq"]})
        self.assertEqual(recovered.display_restore_patch(original), {})
        # Untouched fields survive the patch and remain visible to final comparison.
        restored = {**current, **recovered.display_restore_patch(current)}
        self.assertEqual(restored["bogey"], 0x07E0)
        self.assertEqual(state.differences(original, restored), ["bogey"])

    def test_display_recovery_accepts_each_owned_checkpoint_but_refuses_third_value(self):
        original = snapshot()["display"]
        for color in (0xF800, 0x07E0):
            self.journal.display_intent({"freq": color})
        for color in (original["freq"], 0xF800, 0x07E0):
            self.journal.display_restore_patch({**original, "freq": color})
        with self.assertRaises(state.Incomplete):
            self.journal.display_restore_patch({**original, "freq": 0x001F})

    def test_initial_display_change_cannot_adopt_and_overwrite_newer_setting(self):
        original = snapshot()["display"]
        self.journal.validate_display_change(original, {"freq": 0xF800})
        # Unrelated changed settings do not authorize overwriting a target field.
        self.journal.validate_display_change({**original, "bogey": 0x07E0}, {"freq": 0xF800})
        with self.assertRaises(state.Incomplete):
            self.journal.validate_display_change({**original, "freq": 0x001F}, {"freq": 0xF800})
        self.journal.display_intent({"freq": 0xF800})
        self.journal.validate_display_change({**original, "freq": 0xF800}, {"freq": 0x07E0})
        self.assertEqual(self.reload().value["display_intents"], [{"freq": 0xF800}])

    def test_unsupported_and_malformed_intents_are_never_persisted(self):
        before = deepcopy(self.journal.value)
        for value in ({}, {"unexpected": 1}, {"freq": True}, {"brightness": 0}):
            with self.subTest(value=value), self.assertRaises(ValueError):
                self.journal.display_intent(value)
        for slot, value in ((True, 1), (3, 1), (0, 6), (0, False)):
            with self.subTest(slot=slot, value=value), self.assertRaises(state.Incomplete):
                self.journal.persistence_intent(slot, value)
        self.assertEqual(self.reload().value, before)

    def test_persistence_recovery_preserves_unowned_rows_and_metadata(self):
        baseline = snapshot()["slots"]
        self.journal.persistence_intent(0, 3)
        current = deepcopy(baseline)
        current["slots"][0]["alertPersist"] = 3
        current["slots"][0]["name"] = "CHANGED"
        current["slots"][1]["alertPersist"] = 5
        self.assertEqual(self.reload().persistence_restore(current), [(0, 0)])
        self.assertEqual(self.reload().persistence_restore(baseline), [])

    def test_persistence_recovery_refuses_profile_slot_or_value_drift(self):
        self.journal.persistence_intent(0, 3)
        for change in (lambda s: s.update(activeSlot=1), lambda s: s.update(enabled=False),
                       lambda s: s["slots"][0].update(profile="Other"),
                       lambda s: s["slots"][0].update(alertPersist=4)):
            current = snapshot()["slots"]
            change(current)
            with self.assertRaises(state.Incomplete):
                self.journal.persistence_restore(current)

    def test_initial_persistence_change_cannot_adopt_newer_value_or_profile(self):
        original = snapshot()["slots"]
        self.journal.validate_persistence_change(original, 0)
        for change in (lambda s: s["slots"][0].update(alertPersist=3),
                       lambda s: s["slots"][0].update(profile="Other")):
            current = deepcopy(original)
            change(current)
            with self.assertRaises(state.Incomplete):
                self.journal.validate_persistence_change(current, 0)
        self.journal.persistence_intent(0, 3)
        current = deepcopy(original)
        current["slots"][0]["alertPersist"] = 3
        self.journal.validate_persistence_change(current, 0)

    def test_malformed_disk_journal_is_rejected_before_recovery(self):
        original = deepcopy(self.journal.value)
        mutations = [
            lambda v: v.update(schema=True), lambda v: v.pop("display_intents"),
            lambda v: v.update(identity={"git": None, "image": None, "mode": "normal"}),
            lambda v: v["identity"].update(mode="invalid"),
            lambda v: v.update(usb_identity=None), lambda v: v.update(usb_identity=["unknown", "x", 1, 2]),
            lambda v: v["baseline"]["display"].update(freq=0xF800),
            lambda v: v["display_intents"].append({"stealthEnabled": True}),
            lambda v: v["display_intents"].append({"brightness": False}),
            lambda v: v["persistence_intents"].append({"slot": -1, "value": 3}),
            lambda v: v["persistence_intents"].append({"slot": 0, "value": 3, "profile": "Other"}),
            lambda v: v.update(restoration="anything"),
        ]
        for mutation in mutations:
            value = deepcopy(original)
            mutation(value)
            self.journal.path.write_text(json.dumps(value))
            with self.subTest(value=value.get("schema")), self.assertRaises(state.Incomplete):
                self.reload()

    def test_duplicate_create_and_linked_journal_are_refused(self):
        with self.assertRaises(state.Incomplete):
            state.RecoveryJournal.create(self.directory, IDENTITY, snapshot(), profiles(), USB_IDENTITY)
        saved = self.directory / "saved.json"
        self.journal.path.rename(saved)
        self.journal.path.symlink_to(saved)
        with self.assertRaises(state.Incomplete):
            self.reload()
        with self.assertRaises(state.Incomplete):
            state.atomic_json(self.journal.path, {})

    def test_saved_connection_is_validated_before_any_recovery(self):
        self.journal.save(connection={"base_url": "http://fixture.local:8080", "ssid": "Fixture Bench"})
        self.assertEqual(self.reload().value["connection"]["ssid"], "Fixture Bench")
        for connection in (None, {}, {"base_url": "http://fixture.local", "ssid": ""},
                           {"base_url": "http://user@fixture.test", "ssid": "Fixture"},
                           {"base_url": "http://fixture.local/path", "ssid": "Fixture"},
                           {"base_url": "http://fixture.local", "ssid": "a" * 33}):
            with self.subTest(connection=connection), self.assertRaises(state.Incomplete):
                self.journal.save(connection=connection)


class SemanticTests(unittest.TestCase):
    def test_order_timestamps_and_nvs_namespace_are_the_only_normalizations(self):
        before, after = snapshot(), snapshot()
        after["backup"].update(_timestamp=20, timestamp=20)
        after["backup"]["profiles"].reverse()
        after["device"]["nvsDiag"]["ns"] = "settings-b"
        after["devices"]["devices"].reverse()
        self.assertEqual(state.semantic_snapshot(before), state.semantic_snapshot(after))
        self.assertEqual(before["backup"]["profiles"][0]["name"], "Zulu")
        for section, key, value, path in (
            ("backup", "unrelatedSetting", 9, "backup.unrelatedSetting"),
            ("device", "proxy_name", "Other", "device.proxy_name"),
        ):
            changed = deepcopy(after)
            changed[section][key] = value
            self.assertEqual(state.differences(state.semantic_snapshot(before), state.semantic_snapshot(changed)), [path])
        after["device"]["nvsDiag"]["healthy"] = False
        after["devices"]["devices"][0]["connected"] = True
        self.assertEqual(state.differences(state.semantic_snapshot(before), state.semantic_snapshot(after)),
                         ["device.nvsDiag.healthy", "devices.devices[0].connected"])

    def test_missing_partial_or_malformed_snapshot_refuses_all_writes(self):
        for change in (
            lambda s: s.pop("devices"), lambda s: s.update(slots=[]),
            lambda s: s["slots"].update(slots=[{}, {}, {}]),
            lambda s: s["slots"]["slots"][0].update(alertPersist=True),
            lambda s: s["backup"].update(profiles=[{}, {}]),
            lambda s: s["device"].pop("nvsDiag"),
            lambda s: s["devices"].update(count=3),
            lambda s: s["devices"]["devices"].append(s["devices"]["devices"][0]),
            lambda s: s["quiet"].update(extra=float("nan")),
        ):
            value = snapshot()
            change(value)
            with self.assertRaises(state.Incomplete):
                state.validate_snapshot(value)

    def test_missing_identity_never_equals_another_missing_identity(self):
        for actual, expected in (({}, {}), ({"git": None, "image": None}, {"git": None, "image": None}),
                                 (IDENTITY, {**IDENTITY, "image": "bad"}),
                                 (IDENTITY, {**IDENTITY, "git": "7654321"})):
            with self.assertRaises(state.Incomplete):
                state.verify_identity(actual, expected)
        state.verify_identity(IDENTITY, IDENTITY)

    def test_incomplete_evidence_cannot_publish_pass(self):
        self.assertEqual(state.result_status([{"result": "PASS"}], completed=True, restoration="verified"), "PASS")
        for checks, complete, restoration, error in (([], True, "verified", None),
                ([{"result": "PASS"}], False, "verified", None),
                ([{"result": "PASS"}], True, "unverified", None),
                ([{"result": "PASS"}], True, "verified", "lost"),
                ([{"result": "INCOMPLETE"}], True, "verified", None)):
            self.assertEqual(state.result_status(checks, completed=complete, restoration=restoration, error=error), "INCOMPLETE")


class DeviceTests(unittest.TestCase):
    def device(self, mode):
        device = state.HttpDevice("http://localhost:18765")
        current = snapshot()["slots"]
        history = []

        def request(endpoint, fields=None):
            history.append((endpoint, deepcopy(fields)))
            if fields is not None:
                if mode != "ignore":
                    current["slots"][fields["slot"]]["alertPersist"] = fields["alertPersist"]
                if mode == "lost":
                    raise TimeoutError("fixture lost reply after commit")
                if mode == "lost_without_commit":
                    current["slots"][fields["slot"]]["alertPersist"] = 0
                    raise TimeoutError("fixture lost reply before commit")
                return {"success": True}
            return deepcopy(current)

        device.request = request
        return device, current, history

    def test_uncertain_persistence_response_is_read_back_without_retry(self):
        for mode in ("normal", "lost"):
            device, _, history = self.device(mode)
            device.persistence(0, snapshot()["slots"], 3)
            self.assertEqual([method for _, method in history if method is not None],
                             [{"slot": 0, "profile": "", "alertPersist": 3, "clearProfile": True}])

    def test_unapplied_or_unconfirmed_persistence_cannot_pass(self):
        for mode, expected in (("ignore", state.BehaviorFailure), ("lost_without_commit", state.Incomplete)):
            device, _, history = self.device(mode)
            with self.assertRaises(expected):
                device.persistence(0, snapshot()["slots"], 3)
            self.assertEqual(len([1 for _, fields in history if fields is not None]), 1)

    def test_initial_drift_or_bad_argument_never_writes(self):
        device, current, history = self.device("normal")
        current["slots"][1]["color"] = 77
        with self.assertRaises(state.Incomplete):
            device.persistence(0, snapshot()["slots"], 3)
        for slot, value in ((True, 3), (3, 3), (0, -1)):
            with self.assertRaises(state.Incomplete):
                device.persistence(slot, snapshot()["slots"], value)
        self.assertEqual([fields for _, fields in history if fields is not None], [])

    def test_http_redirect_and_unexpected_url_are_refused(self):
        with self.assertRaises(state.Incomplete):
            state._NoRedirect().redirect_request(None, None, 302, "Found", {}, "http://other.invalid/")
        device = state.HttpDevice("http://localhost:18765")
        for endpoint in ("https://other.invalid/", "/api/display/settings?reset=1", "/api/../settings"):
            with self.assertRaises(ValueError):
                device.request(endpoint, {})

    def test_http_incomplete_or_excessive_readback_is_rejected(self):
        device = state.HttpDevice("http://localhost:18765")
        for raw in (b"[]", b'{"error":"fixture"}', b'{"success":false}', b" " * 524289):
            with patch.object(device.opener, "open", return_value=io.BytesIO(raw)):
                with self.assertRaises(state.Incomplete):
                    device.request("/api/display/settings")

    def test_profile_delete_encodes_the_shipping_json_contract_once(self):
        device = state.HttpDevice("http://localhost:18765")
        with patch.object(device.opener, "open", return_value=io.BytesIO(b'{"success":true}')) as opened:
            self.assertEqual(device.request("/api/v1/profile/delete", {"name": "Road + Test"}), {"success": True})
        self.assertEqual(opened.call_count, 1)
        request = opened.call_args.args[0]
        self.assertEqual(request.get_method(), "POST")
        self.assertEqual(request.get_header("Content-type"), "application/json")
        self.assertEqual(request.get_header("X-v1simple-request"), "maintenance-ui")
        self.assertEqual(json.loads(request.data), {"name": "Road + Test"})
        with patch.object(device.opener, "open") as opened:
            for fields in ({}, {"name": ""}, {"name": "../other"}, {"name": "Road", "other": True}):
                with self.assertRaises(ValueError):
                    device.request("/api/v1/profile/delete", fields)
            opened.assert_not_called()

    def test_existing_setting_endpoints_keep_exact_form_encoding(self):
        device = state.HttpDevice("http://localhost:18765")
        for endpoint, fields, expected in (
                ("/api/display/settings", {"freq": 42}, {"freq": ["42"]}),
                ("/api/audio/settings", {"voiceVolume": 97}, {"voiceVolume": ["97"]}),
                ("/api/autopush/activate", {"slot": 0, "enable": False}, {"slot": ["0"], "enable": ["false"]}),
                ("/api/autopush/slot", {"slot": 0, "alertPersist": 3}, {"slot": ["0"], "alertPersist": ["3"]})):
            with self.subTest(endpoint=endpoint), patch.object(device.opener, "open", return_value=io.BytesIO(b'{"success":true}')) as opened:
                device.request(endpoint, fields)
                self.assertEqual(opened.call_count, 1)
                request = opened.call_args.args[0]
                self.assertEqual(request.get_header("Content-type"), "application/x-www-form-urlencoded")
                self.assertEqual(parse_qs(request.data.decode()), expected)

    def test_delete_http_rejection_retains_actual_status_and_body_without_resend(self):
        device = state.HttpDevice("http://localhost:18765")
        error = urllib.error.HTTPError(device.base_url + "/api/v1/profile/delete", 400, "Bad Request", {},
                                       io.BytesIO(b'{"error":"Invalid JSON"}'))
        with patch.object(device.opener, "open", side_effect=error) as opened:
            with self.assertRaises(state.DeviceHttpError) as caught:
                device.request("/api/v1/profile/delete", {"name": "Road"})
            self.assertEqual(opened.call_count, 1)
        self.assertEqual(caught.exception.status, 400)
        self.assertEqual(caught.exception.payload, {"error": "Invalid JSON"})


class SnapshotReadinessTests(unittest.TestCase):
    def setUp(self):
        self.device = state.HttpDevice("http://localhost:18765")
        self.clock, self.history = 0.0, []
        self.actions = ["busy", "ready"]
        self.failure_endpoint = state.ENDPOINTS["backup"]
        self.failure_status = 503
        self.failure_body = {"success": False, "error": "backup_snapshot_incomplete", "retryable": True}
        self.addCleanup(patch.stopall)
        patch.object(state.time, "monotonic", side_effect=lambda: self.clock).start()
        patch.object(state.time, "sleep", side_effect=self.sleep).start()
        patch.object(self.device.opener, "open", side_effect=self.open).start()

    def sleep(self, seconds):
        self.clock += seconds

    def open(self, request, timeout):
        path = urlsplit(request.full_url).path
        self.history.append((request.get_method(), path, timeout, self.clock))
        self.clock += .01
        if path == self.failure_endpoint:
            action = self.actions.pop(0) if len(self.actions) > 1 else self.actions[0]
            if action == "busy":
                raise urllib.error.HTTPError(request.full_url, self.failure_status, "fixture", {},
                                               io.BytesIO(json.dumps(self.failure_body).encode()))
            if action == "late":
                self.clock += timeout
            if action == "network":
                raise TimeoutError("fixture transport timeout")
        key = next(key for key, endpoint in state.ENDPOINTS.items() if endpoint == path)
        return io.BytesIO(json.dumps(snapshot()[key]).encode())

    def test_shipping_retryable_backup_then_ready_returns_complete_snapshot_with_gets_only(self):
        self.assertEqual(self.device.snapshot(timeout_s=2), snapshot())
        self.assertEqual([row[1] for row in self.history],
                         [state.ENDPOINTS["backup"], *state.ENDPOINTS.values()])
        self.assertTrue(all(row[0] == "GET" for row in self.history))
        for _, _, budget, started in self.history:
            self.assertLessEqual(budget, 2 - started)

    def test_perpetual_retryable_backup_stops_at_one_fixed_deadline(self):
        self.actions = ["busy"]
        with self.assertRaisesRegex(state.Incomplete, "deadline"):
            self.device.snapshot(timeout_s=.6)
        self.assertAlmostEqual(self.clock, .6)
        self.assertTrue(all(row[0:2] == ("GET", state.ENDPOINTS["backup"]) for row in self.history))

    def test_other_status_body_endpoint_or_transport_failure_is_not_retried(self):
        scenarios = (
            (503, {**self.failure_body, "retryable": False}, "backup", "busy"),
            (503, {**self.failure_body, "retryable": 1}, "backup", "busy"),
            (503, {**self.failure_body, "success": 0}, "backup", "busy"),
            (503, {"error": "backup_snapshot_incomplete"}, "backup", "busy"),
            (503, {**self.failure_body, "error": "backup_snapshot_allocation_failed"}, "backup", "busy"),
            (503, {**self.failure_body, "unknown": True}, "backup", "busy"),
            (500, self.failure_body, "backup", "busy"),
            (503, self.failure_body, "display", "busy"),
            (503, self.failure_body, "backup", "network"),
        )
        for status, body, key, action in scenarios:
            with self.subTest(status=status, body=body, endpoint=key, action=action):
                self.history.clear()
                self.clock = 0
                self.failure_status, self.failure_body = status, body
                self.failure_endpoint = state.ENDPOINTS[key]
                self.actions = [action, "ready"]
                with self.assertRaises((state.DeviceHttpError, TimeoutError)):
                    self.device.snapshot(timeout_s=2)
                self.assertEqual(sum(row[1] == self.failure_endpoint for row in self.history), 1)

    def test_late_backup_or_late_final_snapshot_response_cannot_authorize_success(self):
        for key in ("backup", "devices"):
            with self.subTest(endpoint=key):
                self.history.clear()
                self.clock = 0
                self.failure_endpoint = state.ENDPOINTS[key]
                self.actions = ["late"]
                with self.assertRaisesRegex(state.Incomplete, "deadline"):
                    self.device.snapshot(timeout_s=2)

    def test_direct_write_with_same_error_is_never_polled(self):
        self.actions = ["busy", "ready"]
        with self.assertRaises(state.DeviceHttpError):
            self.device.request(state.ENDPOINTS["backup"], {})
        self.assertEqual(len(self.history), 1)
        self.assertEqual(self.history[0][0], "POST")


class ReadinessTests(unittest.TestCase):
    def setUp(self):
        from usb_profiles import default_in_the_box
        self.device = state.HttpDevice("http://localhost:18765")
        self.clock = 0.0
        self.name = "Road + Test"
        self.current = snapshot()["slots"]
        self.current["slots"][0]["profile"] = self.name
        self.expected = deepcopy(self.current)
        self.profile = {"schemaVersion": 4, "name": self.name, "description": "Fixture",
                        "settings": {"bytes": [1, 2, 3, 4, 5, 6]},
                        "detector": {"userSettings": "unchanged", "mode": {"policy": "unchanged"},
                                     "display": "unchanged", "volume": {"policy": "unchanged"},
                                     "bluetoothLed": "unchanged", "customFrequencies": {"policy": "unchanged"}},
                        "inTheBox": default_in_the_box()}
        self.readiness = ["busy", "ready"]
        self.post_mode = "success"
        self.history = []
        self.addCleanup(patch.stopall)
        patch.object(state.time, "monotonic", side_effect=lambda: self.clock).start()
        patch.object(state.time, "sleep", side_effect=self.sleep).start()
        patch.object(self.device.opener, "open", side_effect=self.open).start()

    def sleep(self, seconds):
        self.clock += seconds

    def response(self, value):
        return io.BytesIO(json.dumps(value).encode())

    def rejection(self, url, status, value):
        return urllib.error.HTTPError(url, status, "fixture rejection", {}, self.response(value))

    def open(self, request, timeout):
        method = request.get_method()
        self.history.append((method, request.full_url, timeout, self.clock))
        path = urlsplit(request.full_url).path
        if method == "GET" and path == "/api/v1/profile":
            self.assertEqual(parse_qs(urlsplit(request.full_url).query), {"name": [self.name]})
            action = self.readiness.pop(0) if len(self.readiness) > 1 else self.readiness[0]
            self.clock += .05
            if action == "busy":
                raise self.rejection(request.full_url, 409, {"error": "Profile storage busy; retry"})
            if action == "other409":
                raise self.rejection(request.full_url, 409, {"error": "Profile is corrupt"})
            if action == "extra409":
                raise self.rejection(request.full_url, 409, {"error": "Profile storage busy; retry", "unknown": True})
            if action == "drift":
                self.current["slots"][0]["color"] = 99
            if action == "late":
                self.clock += timeout
            if action == "wrong_name":
                return self.response({**self.profile, "name": "Other"})
            if action == "wrong_schema":
                return self.response({**self.profile, "schemaVersion": 3})
            return self.response(self.profile)
        if method == "GET" and path == "/api/autopush/slots":
            return self.response(self.current)
        if method == "POST" and path == "/api/autopush/slot":
            fields = parse_qs(request.data.decode())
            self.assertEqual(set(fields), {"slot", "profile", "alertPersist"})
            if self.post_mode == "reject":
                raise self.rejection(request.full_url, 409, {"error": "Profile storage busy; retry"})
            self.current["slots"][0]["alertPersist"] = int(fields["alertPersist"][0])
            if self.post_mode == "lost":
                raise TimeoutError("fixture lost committed response")
            return self.response({"success": True})
        raise AssertionError("Unexpected fixture request")

    def posts(self):
        return [row for row in self.history if row[0] == "POST"]

    def test_busy_profile_get_then_ready_permits_one_post_after_fresh_slots(self):
        self.device.persistence(0, self.expected, 3)
        self.assertEqual([urlsplit(row[1]).path for row in self.history],
                         ["/api/v1/profile", "/api/v1/profile", "/api/autopush/slots",
                          "/api/autopush/slot", "/api/autopush/slots"])
        self.assertEqual(len(self.posts()), 1)

    def test_perpetual_busy_has_fixed_budget_and_never_posts(self):
        self.readiness = ["busy"]
        with self.assertRaisesRegex(state.Incomplete, "deadline"):
            self.device.wait_profile_readable(self.name, timeout_s=.6)
        self.assertAlmostEqual(self.clock, .6)
        self.assertEqual(self.posts(), [])
        for _, _, timeout, started in self.history:
            self.assertLessEqual(timeout, .6 - started)
        self.clock = 0.0
        self.history.clear()
        with self.assertRaisesRegex(state.Incomplete, "deadline"):
            self.device.persistence(0, self.expected, 3)
        self.assertAlmostEqual(self.clock, 30.0)
        self.assertEqual(self.posts(), [])

    def test_other_409_or_extra_error_content_stops_without_retry_or_post(self):
        for action in ("other409", "extra409"):
            self.history.clear()
            self.readiness = [action, "ready"]
            with self.assertRaises(state.DeviceHttpError):
                self.device.persistence(0, self.expected, 3)
            self.assertEqual(len(self.history), 1)
            self.assertEqual(self.posts(), [])

    def test_slot_drift_while_waiting_prevents_post(self):
        self.readiness = ["busy", "drift"]
        with self.assertRaisesRegex(state.Incomplete, "changed before"):
            self.device.persistence(0, self.expected, 3)
        self.assertEqual(self.posts(), [])

    def test_success_after_deadline_or_wrong_profile_cannot_authorize_post(self):
        for action in ("late", "wrong_name", "wrong_schema"):
            self.history.clear()
            self.readiness = [action]
            with self.assertRaises(state.Incomplete):
                self.device.wait_profile_readable(self.name, timeout_s=.5)
            self.assertEqual(self.posts(), [])
            self.assertEqual(len(self.history), 1)

    def test_lost_post_response_uses_readback_without_post_retry(self):
        self.post_mode = "lost"
        self.device.persistence(0, self.expected, 3)
        self.assertEqual(len(self.posts()), 1)
        self.assertEqual(self.current["slots"][0]["alertPersist"], 3)

    def test_rejected_post_keeps_status_and_body_without_retry(self):
        self.post_mode = "reject"
        with self.assertRaisesRegex(state.Incomplete, 'HTTP 409.*Profile storage busy; retry'):
            self.device.persistence(0, self.expected, 3)
        self.assertEqual(len(self.posts()), 1)
        self.assertEqual(self.current, self.expected)

    def test_only_exact_read_only_profile_query_is_allowed(self):
        for endpoint, fields in (("/api/v1/profile?name=Road&name=Other", None),
                                  ("/api/v1/profile?name=Road&extra=1", None),
                                  ("/api/v1/profile?name=Road", {}),
                                  ("/api/autopush/slots?name=Road", None),
                                  ("/api/v1/profile?name=../Other", None)):
            with self.assertRaises(ValueError):
                self.device.request(endpoint, fields)
        self.assertEqual(self.history, [])

    def test_rejection_details_are_bounded_and_exclude_unrelated_content(self):
        error = state.DeviceHttpError("/api/v1/profile?name=PrivateName", 409,
                                      {"error": "x" * 1000, "secret": "unrelated"})
        self.assertEqual(len(error.payload["error"]), 240)
        self.assertNotIn("PrivateName", str(error))
        self.assertNotIn("unrelated", str(error))


if __name__ == "__main__":
    unittest.main()
