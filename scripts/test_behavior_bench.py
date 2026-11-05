"""Run the real host coordinator against modeled transports, never a DUT.

These checks exercise admission, durable intent, and recovery after independent
camera/browser/replay failures. Pixel and firmware behavior need their own tests.
"""
from contextlib import ExitStack, nullcontext, redirect_stderr, redirect_stdout
from copy import deepcopy
import json
import io
import hashlib
import os
from pathlib import Path
import runpy
import subprocess
import sys
import tempfile
from types import MethodType, SimpleNamespace
import unittest
from unittest.mock import Mock, patch

import behavior_bench as bench
from test_behavior_state import snapshot, profiles, USB_IDENTITY
from test_usb_profiles import Clock, StartingWireDevice, WireDevice


IDENTITY = {"git": "123abcd", "image": "abcdef123", "mode": "normal", "boot": 1,
            "slot": 0, "persist": 0, "enabled": True, "busy": False}


class Fixture:
    def __init__(self, directory, failures=(), suite="colors"):
        self.directory = directory
        self.suite = suite
        self.failures = set(failures)
        self.events = []
        self.config = snapshot()
        self.documents = {}
        if suite == "profiles":
            self.config["backup"]["profiles"] = []
        self.initial = deepcopy(self.config)
        self.status_value = deepcopy(IDENTITY)
        self.runner = None
        owner = self

        class Http(bench.HttpDevice):
            def request(self, endpoint, fields=None, *, timeout_s=None):
                if fields is not None:
                    if endpoint == "/api/display/settings":
                        owner.events.append("restore_write")
                        if "restore" in owner.failures:
                            raise OSError("fixture restore response lost")
                        owner.config["display"].update(fields)
                    elif endpoint == "/api/v1/profile/delete":
                        owner.events.append("recovery_delete")
                        owner.documents.pop(fields["name"])
                    elif endpoint != "/api/display/preview/clear":
                        raise AssertionError("Unplanned fixture write: " + endpoint)
                    return {"success": True}
                from behavior_state import ENDPOINTS
                key = next(key for key, value in ENDPOINTS.items() if value == endpoint)
                return deepcopy(owner.config[key])

            def wait_profile_readable(self, name, **kwargs):
                if name not in owner.documents:
                    raise bench.DeviceHttpError("/api/v1/profile", 404, {"error": "not found"})
                return deepcopy(owner.documents[name])

            def snapshot(self):
                owner.events.append("snapshot")
                if "snapshot" in owner.failures:
                    raise bench.Incomplete("fixture partial baseline")
                if owner.suite == "profiles":
                    owner.config["backup"]["profiles"] = [{"name": name} for name in owner.documents]
                return super().snapshot()

        class Link:
            def __init__(self, port, output, identity=None):
                self.identity = deepcopy(USB_IDENTITY)

            def finish(self):
                owner.events.append("usb_close")
                if "usb_close" in owner.failures:
                    raise OSError("fixture USB close")

        class Device:
            def __init__(self, client):
                self.client = client
                self.mode_timeout = 45
                if "startup_unavailable" in owner.failures:
                    self.client.clock = Clock()
                    self.client.sleep = self.client.clock.sleep

            def status(self, **kwargs):
                if "startup_unavailable" in owner.failures:
                    owner.events.append("status_probe")
                    self.client.sleep(kwargs["timeout"])
                    raise bench.usb.TransportError("fixture startup unavailable")
                return deepcopy(owner.status_value)

            def mode(self, target, original):
                owner.events.append("mode:" + target)
                if owner.status_value["mode"] != target:
                    owner.status_value["mode"] = target
                    owner.status_value["boot"] += 1
                return self.status()

            def backup(self):
                owner.events.append("profiles_backup")
                if "profiles_backup" in owner.failures:
                    raise bench.Incomplete("fixture partial USB backup")
                bundle = profiles()
                bundle["profiles"] = [{**{key: doc[key] for key in ("schemaVersion", "name", "description", "detector", "inTheBox")},
                                        "rawBytes": doc["settings"]["bytes"]} for doc in owner.documents.values()]
                return json.dumps(bundle).encode(), bundle

        class Camera:
            def __init__(self, directory):
                owner.events.append("camera_create")

            def start(self):
                owner.events.append("camera_start")
                if "camera_start" in owner.failures:
                    raise bench.Incomplete("fixture camera unavailable")

            def finish(self, completed, *, after_recording=None):
                owner.events.append("camera_close")
                if "camera_close" in owner.failures:
                    raise KeyboardInterrupt("fixture camera cleanup interrupt")
                if after_recording:
                    after_recording()
                owner.events.append("camera_verify")
                if "camera_verify" in owner.failures:
                    raise OSError("fixture slow video verification failed")
                return {"result": "CAPTURED"}

        class Browser:
            def __init__(self, *args, **kwargs):
                pass

            def __enter__(self):
                return self

            def __exit__(self, *args):
                owner.events.append("browser_close")
                if "browser_close" in owner.failures:
                    raise RuntimeError("fixture browser cleanup")

            def save_display(self, change, expected_before):
                # This is the actual point at which an external setting write
                # would occur. Reopen the on-disk record before changing state.
                journal = bench.RecoveryJournal.load(owner.directory)
                assert journal.value["baseline"] == owner.initial
                assert journal.value["display_intents"][-1] == change
                assert journal.value["stage"] == "display_write_pending"
                assert owner.config["display"] == expected_before
                owner.events.append("browser_write")
                if "browser_false_success" not in owner.failures:
                    owner.config["display"].update(change)
                if "browser_uncertain" in owner.failures:
                    return {"result": "INCOMPLETE"}
                return {"result": "PASS"}

            def prepare_profile_collision(self, document, catalog_names):
                owner.events.append("collision_prepare")
                assert set(catalog_names) == set(owner.documents)
                return deepcopy(document)

            def profile_create(self, document, catalog_names):
                saved = bench.RecoveryJournal.load(owner.directory)
                assert saved.value["profile_intents"][-1] == {"action": "create", "before": None, "after": document}
                assert document["name"] not in owner.documents
                owner.events.append("profile_create")
                owner.documents[document["name"]] = deepcopy(document)
                return {"result": "PASS", "browser_closed": True}

            def profile_edit(self, before, after):
                saved = bench.RecoveryJournal.load(owner.directory)
                assert saved.value["profile_intents"][-1] == {"action": "edit", "before": before, "after": after}
                assert owner.documents[before["name"]] == before
                owner.events.append("profile_edit")
                owner.documents[after["name"]] = deepcopy(after)
                if "profile_foreign_edit" in owner.failures:
                    owner.documents[after["name"]]["description"] = "external edit"
                return {"result": "INCOMPLETE" if "profile_edit_lost" in owner.failures else "PASS", "browser_closed": True}

            def profile_collision(self, token):
                owner.events.append("profile_collision")
                assert owner.documents[token["name"]]["description"] != token["description"]
                return {"result": "PASS", "browser_closed": True}

            def discard_profile_collision(self, token):
                owner.events.append("collision_close")
                if "collision_close" in owner.failures:
                    raise RuntimeError("fixture stale page did not close")

            def profile_reload(self, expected):
                assert owner.documents[expected["name"]] == expected
                owner.events.append("profile_reload")
                return {"result": "PASS", "browser_closed": True}

            def profile_delete(self, expected):
                saved = bench.RecoveryJournal.load(owner.directory)
                assert saved.value["profile_intents"][-1] == {"action": "delete", "before": expected, "after": None}
                owner.events.append("profile_delete")
                if "profile_delete_failed" not in owner.failures:
                    del owner.documents[expected["name"]]
                return {"result": "INCOMPLETE" if "profile_delete_failed" in owner.failures else "PASS", "browser_closed": True}

        self.Http, self.Link, self.Device, self.Camera, self.Browser = Http, Link, Device, Camera, Browser

    def colors(self, runner):
        if "prewrite_drift" in self.failures:
            self.config["display"]["freq"] = 0x001F
        runner.save_display({"freq": 0xF800}, "fixture-save")
        if "replay_close" in self.failures:
            def finish(completed):
                self.events.append("replay_close")
                raise RuntimeError("fixture replay cleanup")
            runner.replay = SimpleNamespace(finish=finish)
        if "unowned_display" in self.failures:
            self.config["display"]["bogey"] = 0x07E0
        if "unowned_owned_field" in self.failures:
            self.config["display"]["freq"] = 0x001F
        if "proven_failure" in self.failures:
            raise bench.BehaviorFailure("fixture proved contradiction")

    def execute(self, *, command="run", analysis=None, audio_analysis=None, base_url="http://localhost:18765", ssid="Fixture"):
        args = SimpleNamespace(command=command, base_url=base_url, port=None,
                               git=IDENTITY["git"], image=IDENTITY["image"], suite=self.suite,
                               tolerance=.35, ssid=ssid, headed=False)
        real_atomic = bench.atomic_json
        real_save = bench.RecoveryJournal.save

        def atomic(path, value):
            if "capture_record" in self.failures and path.name == "observations.json":
                raise OSError("fixture capture record disk failure")
            return real_atomic(path, value)

        def save(journal, **updates):
            if "recovery_record" in self.failures and updates.get("restoration") == "unverified":
                raise OSError("fixture failed recovery record")
            return real_save(journal, **updates)

        def analyze(_directory, *, tolerance, checks=None):
            if self.suite in ("audio", "warning"):
                self.events.append("display_analysis")
                raise AssertionError("audio-only run used the display-suite analyzer")
            if checks is None:
                checks = []
            checks.extend([{"result": "PASS", "check": name} for name in sorted(bench.required_checks(args.suite))]
                          if analysis is None else analysis)
            if "analysis_late_failure" in self.failures:
                raise bench.Incomplete("fixture later capture decode unavailable")
            return checks

        def collect_audio(runner):
            self.events.append("audio_collect")
            runner.save_display({"freq": 0xF800}, "audio_fixture")
            if "audio_collection" in self.failures:
                raise bench.Incomplete("fixture microphone unavailable after setup")
            return []

        def analyze_audio(_directory):
            self.events.append("audio_analysis")
            return ([{"check": name, "result": "PASS"} for name in sorted(bench.required_checks("audio"))]
                    if audio_analysis is None else audio_analysis)

        with ExitStack() as stack:
            for name, value in (("HttpDevice", self.Http), ("LoggedLink", self.Link),
                                ("CaptureSession", self.Camera), ("BrowserDriver", self.Browser),
                                ("atomic_json", atomic), ("progress", lambda _: None)):
                stack.enter_context(patch.object(bench, name, value))
            stack.enter_context(patch.object(bench.usb, "Device", self.Device))
            stack.enter_context(patch.object(bench.RecoveryJournal, "save", save))
            stack.enter_context(patch.object(bench.Runner, "wait", lambda *_: None))
            stack.enter_context(patch.object(bench, "analyze", side_effect=analyze))
            if self.suite == "audio":
                stack.enter_context(patch("behavior_audio_runner.run_audio_cases", side_effect=collect_audio))
                stack.enter_context(patch("behavior_audio_runner.analyze_audio_cases", side_effect=analyze_audio))
            if self.suite == "warning":
                stack.enter_context(patch.object(bench, "analyze_warning", side_effect=lambda *a, **kw:
                    [{"check": name, "result": "PASS"} for name in bench.WARNING_CHECKS]))
            self.runner = bench.Runner(args, self.directory)
            self.runner.colors = MethodType(lambda runner: self.colors(runner), self.runner)
            if self.suite == "audio":
                for name in ("colors", "clearance", "profiles"):
                    setattr(self.runner, name, Mock(side_effect=AssertionError("unselected suite ran")))
            if self.suite == "warning":
                self.runner.warning = MethodType(collect_audio, self.runner)
            return self.runner.execute()


class InitialReadinessTests(unittest.TestCase):
    def runner(self, peer, clock):
        runner = bench.Runner.__new__(bench.Runner)
        runner.device = bench.usb.Device(bench.usb.Client(peer, clock=clock, sleep=clock.sleep))
        runner.expected = {"git": "1234567", "image": "abcdef123"}
        return runner

    def test_slow_storage_boot_is_admitted_without_changing_mutation_limits(self):
        clock = Clock()
        peer = StartingWireDevice(clock, ready_at=23.595)
        runner = self.runner(peer, clock)
        self.assertEqual(runner.initial_status()["mode"], "normal")
        self.assertGreaterEqual(clock(), peer.ready_at)
        self.assertLess(clock(), 27)
        self.assertTrue(all(b" status\n" in raw for _, raw in peer.startup_requests))
        self.assertEqual([verb for _, verb, _ in peer.history], ["status"])
        self.assertEqual(peer.closes, 0)
        self.assertEqual((runner.device.client.timeout, runner.device.client.attempts), (2., 3))
        peer.drop_always.add("maintenance")
        started = clock()
        with self.assertRaises(bench.usb.TransportError):
            runner.device.client.command("maintenance")
        self.assertGreaterEqual(clock() - started, 6.)
        self.assertLess(clock() - started, 6.1)

    def test_shared_deadline_bounds_absence_and_rejects_a_late_complete_reply(self):
        class LateReply(WireDevice):
            def read(self, count):
                self.clock.now = 45.001
                result = bytes(self.output[:count])
                del self.output[:count]
                return result

        for make_peer in (lambda clock: StartingWireDevice(clock, ready_at=100), LateReply):
            with self.subTest(peer=make_peer):
                clock = Clock()
                peer = make_peer(clock)
                runner = self.runner(peer, clock)
                with self.assertRaisesRegex(bench.usb.TransportError, "startup deadline"):
                    runner.initial_status()
                self.assertGreaterEqual(clock(), 45.)
                self.assertLess(clock(), 45.02)
                self.assertTrue(all(verb == "status" for _, verb, _ in peer.history))

    def test_malformed_rejected_and_foreign_status_are_not_retried(self):
        for changed in ({"mode": "invalid"}, {"git": "7654321"}, {"image": "fedcba987"},
                        {"ok": False, "error": "busy"}):
            with self.subTest(changed=changed):
                clock = Clock()
                peer = WireDevice(clock)
                runner = self.runner(peer, clock)
                reply = {**peer.dispatch("status", []), **changed}
                with patch.object(peer, "dispatch", return_value=reply):
                    with self.assertRaises((bench.usb.ProfileError, bench.Incomplete)):
                        runner.initial_status()
                self.assertEqual(len(peer.history), 1)
                self.assertLess(clock(), 1.)

    def test_immediate_transport_absence_has_bounded_backoff(self):
        clock = Clock()
        runner = self.runner(WireDevice(clock), clock)
        with patch.object(runner.device, "status", side_effect=bench.usb.TransportError("absent")) as probe:
            with self.assertRaises(bench.usb.TransportError):
                runner.initial_status()
        self.assertAlmostEqual(clock(), 45.)
        self.assertLessEqual(probe.call_count, 451)
        for call in probe.call_args_list:
            self.assertEqual(call.kwargs["deadline"], 45.)
            self.assertEqual(call.kwargs["attempts"], 1)
            self.assertLessEqual(call.kwargs["timeout"], 2.)


class MaintenanceReadinessTests(unittest.TestCase):
    def runner(self):
        runner = bench.Runner.__new__(bench.Runner)
        runner.status = Mock(return_value=deepcopy(IDENTITY))
        runner.expected = {key: IDENTITY[key] for key in ("git", "image")}
        runner.device = SimpleNamespace(mode=Mock(return_value={**IDENTITY, "mode": "maintenance", "boot": 2}))
        runner.connection = {"ssid": "Fixture"}
        runner.http = bench.HttpDevice("http://localhost:18765")
        return runner

    def test_both_wifi_credentials_fit_and_http_proves_join_when_ssid_is_hidden(self):
        clock, runner = Clock(), self.runner()
        state = {"connected": False, "attempts": []}
        join = bench.try_join_maintenance_wifi

        def execute(command, **kwargs):
            action = command[1]
            credential = action == "-setairportnetwork" and len(command) == 5
            duration = (4.8 if credential else 5.7) if action == "-setairportnetwork" else .025
            if action == "-setairportnetwork":
                state["attempts"].append((credential, kwargs["timeout"]))
            if duration > kwargs["timeout"]:
                clock.sleep(kwargs["timeout"])
                raise subprocess.TimeoutExpired(command, kwargs["timeout"])
            clock.sleep(duration)
            if action == "-listallhardwareports":
                output = "Hardware Port: Wi-Fi\nDevice: en0\n"
            elif action == "-getairportnetwork":
                output = "You are not associated with an AirPort network.\n"
            else:
                state["connected"] = credential
                output = "" if credential else "Failed to join network"
            return SimpleNamespace(returncode=0, stdout=output)

        def request(endpoint, *, timeout_s):
            self.assertEqual(endpoint, "/api/display/settings")
            if not state["connected"]:
                clock.sleep(min(8., timeout_s))
                raise OSError("fixture host is on another network")
            return snapshot()["display"]

        def recover(**kwargs):
            result = join(**kwargs, run=execute, monotonic=clock)
            self.assertFalse(result)  # The host masks SSID even after joining.
            return result

        with patch.object(bench.time, "monotonic", clock), patch.object(bench.time, "sleep", clock.sleep), \
                patch.object(runner.http, "request", side_effect=request), \
                patch.object(bench, "try_join_maintenance_wifi", side_effect=recover) as rejoin:
            self.assertEqual(runner.mode("maintenance")["mode"], "maintenance")
        self.assertEqual([credential for credential, _ in state["attempts"]], [False, True])
        self.assertGreaterEqual(state["attempts"][1][1], 4.8)
        rejoin.assert_called_once_with(ssid="Fixture", timeout_s=30)
        self.assertLess(clock(), 60.)
        runner.device.mode.assert_called_once()

    def test_http_and_rejoin_share_one_total_deadline(self):
        clock, runner = Clock(), self.runner()
        budgets = []
        def request(endpoint, *, timeout_s):
            budgets.append(timeout_s)
            clock.sleep(min(8., timeout_s))
            raise OSError("fixture website unavailable")
        def recover(*, ssid, timeout_s):
            clock.sleep(timeout_s)
            return False
        with patch.object(bench.time, "monotonic", clock), patch.object(bench.time, "sleep", clock.sleep), \
                patch.object(runner.http, "request", side_effect=request), \
                patch.object(bench, "try_join_maintenance_wifi", side_effect=recover) as rejoin:
            with self.assertRaisesRegex(bench.Incomplete, "Maintenance website"):
                runner.mode("maintenance")
        self.assertEqual(clock(), 60.)
        self.assertEqual(budgets, [60., 21.75])
        self.assertEqual([call.kwargs["timeout_s"] for call in rejoin.call_args_list], [30, 13.75])
        runner.device.mode.assert_called_once()

    def test_late_valid_http_response_cannot_admit_maintenance(self):
        clock, runner = Clock(), self.runner()
        def late(endpoint, *, timeout_s):
            self.assertEqual(timeout_s, 60.)
            clock.sleep(60.1)
            return snapshot()["display"]
        with patch.object(bench.time, "monotonic", clock), patch.object(bench.time, "sleep", clock.sleep), \
                patch.object(runner.http, "request", side_effect=late), \
                patch.object(bench, "try_join_maintenance_wifi") as rejoin:
            with self.assertRaisesRegex(bench.Incomplete, "Maintenance website"):
                runner.mode("maintenance")
        rejoin.assert_not_called()
        runner.device.mode.assert_called_once()

    def test_optional_display_timeout_keeps_complete_canonical_validation(self):
        http = bench.HttpDevice("http://localhost:18765")
        with patch.object(http, "request", return_value=snapshot()["display"]) as request:
            self.assertEqual(http.display(timeout_s=.5), bench.canonical_display(snapshot()["display"]))
            request.assert_called_once_with("/api/display/settings", timeout_s=.5)
            request.return_value = {}
            with self.assertRaises(ValueError):
                http.display()


class CoordinatorTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name).resolve()

    def fixture(self, *failures):
        return Fixture(self.directory, failures)

    def test_real_coordinator_journals_before_save_then_restores_original_state(self):
        fixture = self.fixture()
        result = fixture.execute()
        self.assertEqual(result["result"], "PASS")
        self.assertEqual(result["restoration"], "verified")
        self.assertEqual(fixture.config, fixture.initial)
        self.assertEqual(fixture.status_value["mode"], "normal")
        self.assertEqual(fixture.events.count("browser_write"), 1)
        self.assertEqual(fixture.events.count("restore_write"), 1)
        self.assertLess(fixture.events.index("profiles_backup"), fixture.events.index("browser_write"))
        self.assertEqual(fixture.events[-1], "usb_close")

    def test_each_independent_cleanup_failure_still_reaches_restoration_and_usb_close(self):
        for failure in ("camera_close", "camera_verify", "browser_close", "replay_close", "capture_record"):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as directory:
                fixture = Fixture(Path(directory), [failure])
                result = fixture.execute()
                self.assertEqual(result["result"], "INCOMPLETE")
                self.assertEqual(result["restoration"], "verified")
                self.assertEqual(fixture.config, fixture.initial)
                self.assertTrue(result["cleanup_errors"])
                self.assertEqual(fixture.events[-1], "usb_close")

    def test_multiple_cleanup_failures_do_not_mask_restoration(self):
        fixture = self.fixture("camera_close", "browser_close", "replay_close", "capture_record", "usb_close")
        result = fixture.execute()
        self.assertEqual(result["restoration"], "verified")
        self.assertEqual(result["result"], "INCOMPLETE")
        self.assertEqual(len(result["cleanup_errors"]), 5)
        self.assertEqual(fixture.config, fixture.initial)

    def test_restoration_finishes_between_recording_stop_and_video_verification(self):
        for failures in ((), ("camera_verify",), ("restore",)):
            with self.subTest(failures=failures), tempfile.TemporaryDirectory() as directory:
                fixture = Fixture(Path(directory).resolve(), failures)
                result = fixture.execute()
                self.assertLess(fixture.events.index("camera_close"), fixture.events.index("restore_write"))
                self.assertLess(fixture.events.index("restore_write"), fixture.events.index("camera_verify"))
                self.assertEqual(fixture.events.count("restore_write"), 1)
                if "restore" not in failures:
                    self.assertEqual(result["restoration"], "verified")
                    self.assertEqual(fixture.config, fixture.initial)
                    # The complete post-reboot comparison also precedes media work.
                    self.assertLess(max(index for index, value in enumerate(fixture.events)
                                        if value == "profiles_backup"), fixture.events.index("camera_verify"))
                else:
                    self.assertEqual(result["restoration"], "unverified")
                    journal = bench.RecoveryJournal.load(fixture.directory)
                    self.assertEqual(journal.value["restoration_error"],
                                     "Original display fields could not be restored")
                    self.assertEqual(result["cleanup_errors"], [])

    def test_partial_baseline_or_camera_start_cannot_send_a_setting_write(self):
        for failure in ("snapshot", "profiles_backup", "camera_start"):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as directory:
                fixture = Fixture(Path(directory), [failure])
                result = fixture.execute()
                self.assertEqual(result["result"], "INCOMPLETE")
                self.assertNotIn("browser_write", fixture.events)
                self.assertNotIn("restore_write", fixture.events)
                self.assertEqual(fixture.status_value["mode"], "normal")
                self.assertEqual(fixture.events[-1], "usb_close")

    def test_initial_unavailable_or_wrong_identity_cannot_start_admission_writes(self):
        for unavailable in (False, True):
            with self.subTest(unavailable=unavailable), tempfile.TemporaryDirectory() as directory:
                fixture = Fixture(Path(directory), ["startup_unavailable"] if unavailable else [])
                if not unavailable:
                    fixture.status_value["git"] = "7654321"
                result = fixture.execute()
                self.assertEqual(result["result"], "INCOMPLETE")
                self.assertEqual(result["restoration"], "not_needed")
                self.assertIsNone(fixture.runner.journal)
                self.assertFalse((Path(directory) / "recovery.json").exists())
                self.assertTrue(all(event in ("status_probe", "usb_close") for event in fixture.events))
                self.assertEqual(fixture.events[-1], "usb_close")

    def test_uncertain_browser_commit_is_not_retried_and_is_restored(self):
        fixture = self.fixture("browser_uncertain")
        result = fixture.execute()
        self.assertEqual(result["result"], "INCOMPLETE")
        self.assertEqual(fixture.events.count("browser_write"), 1)
        self.assertEqual(fixture.events.count("restore_write"), 1)
        self.assertEqual(fixture.config, fixture.initial)

    def test_independent_readback_catches_false_browser_success(self):
        fixture = self.fixture("browser_false_success")
        result = fixture.execute()
        self.assertEqual(result["result"], "FAIL")
        self.assertEqual(result["restoration"], "verified")
        self.assertEqual(fixture.config, fixture.initial)

    def test_newer_setting_after_backup_is_never_adopted_as_owned(self):
        fixture = self.fixture("prewrite_drift")
        result = fixture.execute()
        self.assertEqual(result["result"], "INCOMPLETE")
        self.assertNotIn("browser_write", fixture.events)
        self.assertNotIn("restore_write", fixture.events)
        self.assertEqual(fixture.config["display"]["freq"], 0x001F)

    def test_recovery_record_failure_does_not_skip_usb_close(self):
        fixture = self.fixture("restore", "recovery_record")
        result = fixture.execute()
        self.assertEqual(result["restoration"], "unverified")
        self.assertEqual(result["result"], "INCOMPLETE")
        self.assertTrue(any(row.startswith("recovery record:") for row in result["cleanup_errors"]))
        self.assertEqual(fixture.events[-1], "usb_close")

    def test_wrong_firmware_identity_prevents_even_read_only_admission(self):
        fixture = self.fixture()
        fixture.status_value["image"] = "123456789"
        result = fixture.execute()
        self.assertEqual(result["result"], "INCOMPLETE")
        self.assertEqual(fixture.events, ["usb_close"])

    def test_unowned_change_is_preserved_and_prevents_success(self):
        fixture = self.fixture("unowned_display")
        result = fixture.execute()
        self.assertEqual(result["result"], "INCOMPLETE")
        self.assertEqual(result["restoration"], "unverified")
        self.assertEqual(fixture.config["display"]["bogey"], 0x07E0)
        self.assertEqual(fixture.config["display"]["freq"], fixture.initial["display"]["freq"])
        self.assertIn("display.bogey", result["error"])

    def test_third_value_in_owned_field_refuses_restore_write(self):
        fixture = self.fixture("unowned_owned_field")
        result = fixture.execute()
        self.assertEqual(result["restoration"], "unverified")
        self.assertNotIn("restore_write", fixture.events)
        self.assertEqual(fixture.config["display"]["freq"], 0x001F)

    def test_failed_restore_command_is_attempted_once_and_link_closes(self):
        first = self.fixture("restore")
        first.execute()
        # A new host invocation resumes the saved journal. The failure below
        # must not trigger an automatic second restore in execute.finally.
        recovered = self.fixture("restore")
        recovered.config["display"]["freq"] = 0xF800
        result = recovered.execute(command="restore")
        self.assertEqual(result["result"], "INCOMPLETE")
        self.assertEqual(result["restoration"], "unverified")
        self.assertEqual(recovered.events.count("restore_write"), 1)
        self.assertEqual(recovered.events[-1], "usb_close")

    def test_proven_failure_remains_failure_after_successful_recovery(self):
        fixture = self.fixture("proven_failure")
        result = fixture.execute()
        self.assertEqual(result["result"], "FAIL")
        self.assertEqual(result["restoration"], "verified")
        self.assertEqual(fixture.config, fixture.initial)

    def test_later_restore_never_replaces_the_qualification_verdict(self):
        for failure, expected in (("proven_failure", "FAIL"), ("browser_uncertain", "INCOMPLETE")):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as directory:
                fixture = Fixture(Path(directory).resolve(), [failure])
                first = fixture.execute()
                original = (fixture.directory / "result.json").read_bytes()
                self.assertEqual(first["result"], expected)
                recovered = fixture.execute(command="restore")
                self.assertEqual(recovered["result"], "PASS")
                self.assertEqual(recovered["operation"], "restore")
                self.assertEqual((fixture.directory / "result.json").read_bytes(), original)
                self.assertEqual(json.loads((fixture.directory / "restoration-result.json").read_text()), recovered)

    def test_restore_reuses_saved_custom_connection_when_not_overridden(self):
        fixture = self.fixture()
        custom = {"base_url": "http://fixture-dut.local:8080", "ssid": "Fixture Bench"}
        self.assertEqual(fixture.execute(**custom)["result"], "PASS")
        journal = bench.RecoveryJournal.load(self.directory)
        self.assertEqual(journal.value["connection"], custom)
        self.assertEqual(fixture.execute(command="restore", base_url=None, ssid=None)["result"], "PASS")
        self.assertEqual(fixture.runner.connection, custom)
        self.assertEqual(fixture.runner.http.base_url, custom["base_url"])

    def test_preview_ignores_transient_busy_but_rejects_an_unexpected_boot(self):
        before = {**IDENTITY, "mode": "maintenance", "busy": False}
        runner = bench.Runner.__new__(bench.Runner)
        runner.directory, runner.plan = self.directory, {"previews": []}
        runner.http = SimpleNamespace(preview_start=lambda: (1, 2), request=lambda *_: {})
        runner.wait = lambda *_: None
        runner.status = Mock(side_effect=[before, {**before, "busy": True}])
        runner.preview("fixture", {"freq": bench.SENTINELS["magenta"]}, bench.BARS_A, False)
        self.assertEqual(len(runner.plan["previews"]), 1)
        runner.status = Mock(side_effect=[before, {**before, "boot": before["boot"] + 1}])
        with self.assertRaises(bench.BehaviorFailure):
            runner.preview("fixture", {"freq": bench.SENTINELS["magenta"]}, bench.BARS_A, False)

    def test_zero_visual_checks_cannot_publish_or_exit_successfully(self):
        result = self.fixture().execute(analysis=[])
        self.assertEqual(result["result"], "INCOMPLETE")
        with patch.object(bench, "analyze", return_value=[]):
            self.assertEqual(bench.main(["analyze", "--out-dir", str(self.directory)]), 2)

    def test_missing_or_duplicate_required_check_cannot_pass(self):
        names = sorted(bench.required_checks("colors"))
        for selected in (names[:-1], names + names[:1], ["invented"]):
            with self.subTest(selected=selected), tempfile.TemporaryDirectory() as directory:
                fixture = Fixture(Path(directory))
                result = fixture.execute(analysis=[{"result": "PASS", "check": name} for name in selected])
                self.assertEqual(result["result"], "INCOMPLETE")
                self.assertEqual(result["restoration"], "verified")

    def test_profile_campaign_uses_journal_before_each_mutation_and_reboots_before_reload(self):
        fixture = Fixture(self.directory, suite="profiles")
        result = fixture.execute()
        self.assertEqual(result["result"], "PASS", result)
        self.assertEqual({row["check"] for row in result["checks"]}, bench.required_checks("profiles"))
        self.assertEqual(fixture.documents, {})
        self.assertNotIn("camera_start", fixture.events)
        self.assertLess(fixture.events.index("collision_close"), fixture.events.index("mode:normal"))
        self.assertLess(fixture.events.index("mode:normal"), fixture.events.index("profile_reload"))
        self.assertEqual(fixture.events.count("profile_delete"), 1)
        self.assertNotIn("recovery_delete", fixture.events)

    def test_profile_uncertain_edit_recovers_once_after_pages_close(self):
        fixture = Fixture(self.directory, ("profile_edit_lost",), suite="profiles")
        result = fixture.execute()
        self.assertEqual(result["result"], "INCOMPLETE")
        self.assertEqual(result["restoration"], "verified")
        self.assertEqual(fixture.documents, {})
        self.assertEqual(fixture.events.count("profile_edit"), 1)
        self.assertEqual(fixture.events.count("recovery_delete"), 1)
        self.assertLess(fixture.events.index("browser_close"), fixture.events.index("recovery_delete"))

    def test_profile_external_edit_and_unresolved_delete_are_preserved_without_retry(self):
        for failure in ("profile_foreign_edit", "profile_delete_failed", "collision_close"):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as directory:
                fixture = Fixture(Path(directory), [failure], suite="profiles")
                result = fixture.execute()
                self.assertNotEqual(result["result"], "PASS")
                if failure == "collision_close":
                    self.assertEqual(result["restoration"], "verified")
                    self.assertLess(fixture.events.index("browser_close"), fixture.events.index("recovery_delete"))
                else:
                    self.assertEqual(result["restoration"], "unverified")
                    self.assertEqual(len(fixture.documents), 1)
                    self.assertNotIn("recovery_delete", fixture.events)

    def test_unconfirmed_profile_browser_shutdown_blocks_device_recovery_writes(self):
        fixture = Fixture(self.directory, ("profile_edit_lost", "browser_close"), suite="profiles")
        result = fixture.execute()
        self.assertEqual(result["result"], "INCOMPLETE")
        self.assertEqual(result["restoration"], "unverified")
        self.assertNotIn("recovery_delete", fixture.events)
        self.assertEqual(len(fixture.documents), 1)
        self.assertEqual(fixture.events[-1], "usb_close")

    def test_recovery_receipts_distinguish_rejection_from_unknown_without_retry(self):
        for fault, outcome in ((bench.DeviceHttpError("/api/v1/profile/delete", 400, {"error": "Invalid JSON"}), "rejected"),
                               (TimeoutError("response lost"), "unknown"), (None, "responded")):
            with self.subTest(outcome=outcome), tempfile.TemporaryDirectory() as directory:
                runner = bench.Runner.__new__(bench.Runner)
                runner.directory = Path(directory)
                runner.http = SimpleNamespace(request=Mock(side_effect=fault, return_value={"success": True}))
                runner.recovery_write("profile-delete", "/api/v1/profile/delete", {"name": "Temporary"})
                runner.http.request.assert_called_once_with("/api/v1/profile/delete", {"name": "Temporary"})
                files = list(runner.directory.glob("recovery-profile-delete-*.json"))
                self.assertEqual(len(files), 1)
                receipt = json.loads(files[0].read_text())
                self.assertEqual(receipt["outcome"], outcome)
                if outcome == "rejected":
                    self.assertEqual(receipt["status"], 400)
                    self.assertEqual(receipt["response"], {"error": "Invalid JSON"})
                elif outcome == "unknown":
                    self.assertEqual(receipt["exception_type"], "TimeoutError")
                    self.assertNotIn("response", receipt)

    def test_audio_suite_has_only_ten_named_acoustic_and_optical_requirements(self):
        expected = {"audio_ka", "audio_ka_visual", "audio_detector_zero", "audio_detector_zero_visual",
                    "audio_x", "audio_x_visual", "audio_speaker_zero", "audio_speaker_zero_visual",
                    "audio_recovery", "audio_recovery_visual"}
        self.assertEqual(bench.required_checks("audio"), expected)
        self.assertTrue(expected <= bench.required_checks("acceptance"))

    def test_warning_scope_preserves_defaults_and_restores_after_collection_failure(self):
        self.assertEqual(bench.required_checks("warning"), set(bench.WARNING_CHECKS))
        self.assertFalse(set(bench.WARNING_CHECKS) & bench.required_checks("all"))
        fixture = Fixture(self.directory, suite="warning")
        result = fixture.execute()
        self.assertEqual(result["result"], "PASS", result)
        self.assertEqual(fixture.config, fixture.initial)
        self.assertIn("camera_start", fixture.events)
        with tempfile.TemporaryDirectory() as temporary:
            fixture = Fixture(Path(temporary), ("audio_collection",), suite="warning")
            result = fixture.execute()
            self.assertEqual(result["result"], "INCOMPLETE")
            self.assertEqual(result["restoration"], "verified")
            self.assertEqual(fixture.config, fixture.initial)

    def test_audio_only_runner_collects_with_camera_then_restores_before_analysis(self):
        fixture = Fixture(self.directory, suite="audio")
        result = fixture.execute()
        self.assertEqual(result["result"], "PASS", result)
        self.assertEqual(result["scope"], "audio")
        self.assertEqual(len(result["checks"]), 10)
        self.assertEqual(fixture.config, fixture.initial)
        self.assertEqual(result["restoration"], "verified")
        self.assertEqual(fixture.runner.plan["previews"], [])
        self.assertEqual(fixture.runner.plan["replays"], [])
        self.assertNotIn("display_analysis", fixture.events)
        self.assertLess(fixture.events.index("camera_start"), fixture.events.index("audio_collect"))
        self.assertLess(fixture.events.index("restore_write"), fixture.events.index("audio_analysis"))
        self.assertLess(fixture.events.index("usb_close"), fixture.events.index("audio_analysis"))

    def test_audio_incomplete_collection_restores_and_missing_checks_cannot_pass(self):
        fixture = Fixture(self.directory, ("audio_collection",), suite="audio")
        result = fixture.execute()
        self.assertEqual(result["result"], "INCOMPLETE")
        self.assertEqual(result["restoration"], "verified")
        self.assertEqual(fixture.config, fixture.initial)
        self.assertNotIn("audio_analysis", fixture.events)
        for missing in ([], [{"check": "audio_ka", "result": "PASS"}]):
            with tempfile.TemporaryDirectory() as directory:
                result = Fixture(Path(directory), suite="audio").execute(audio_analysis=missing)
                self.assertEqual(result["result"], "INCOMPLETE")
                self.assertEqual(result["restoration"], "verified")

    def test_earlier_optical_failure_survives_later_analysis_exception(self):
        result = self.fixture("analysis_late_failure").execute(analysis=[{"result": "FAIL", "check": "early_pixels"}])
        self.assertEqual(result["result"], "FAIL")
        self.assertEqual(result["checks"], [{"result": "FAIL", "check": "early_pixels"}])
        self.assertIn("Visual analysis incomplete", result["error"])


def observation_plan(suite="all"):
    previews = [{"label": name + suffix, "request_before_ns": 1_000_000_000,
                 "request_after_ns": 1_100_000_000, "ended_ns": 40_100_000_000,
                 "expected_frequency_colors": {}, "expected_bars": []}
                for name in ("custom_a", "custom_b", "band_colors")
                for suffix in ("_saved", "_rebooted")] if suite != "clearance" else []
    replays = [{"name": f"{scenario}-{persist}", "scenario": scenario, "persistence_s": persist,
                "result_file": f"{scenario}-{persist}/behavior_replay_result.json",
                "tails": [{}] * (2 if scenario == "clearance" else 1)}
               for persist in (0, 3) for scenario in ("persistence", "clearance")] if suite != "colors" else []
    return {"schema": 1, "suite": suite, "previews": previews, "replays": replays,
            "capture_completed": True, "timing_tolerance_s": .35}


class AcceptanceAdmissionTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name).resolve()

    def candidate(self):
        root = self.directory / "repo"
        (root / ".artifacts").mkdir(parents=True)
        (root / "scripts").mkdir()
        (root / "scripts/behavior_bench.py").write_text("fixture runner")
        (root / "scripts/usb_profiles.py").write_text("fixture USB")
        (root / ".artifacts/ci-gate-passed.sha").write_text("a" * 40)
        retained = self.directory / "retained"
        retained.mkdir()
        elf = b"fixture exact ELF"
        image = hashlib.sha256(elf).hexdigest()
        binary = bytearray(208)
        binary[0] = 0xE9
        binary[176:208] = bytes.fromhex(image)
        (retained / "firmware.bin").write_bytes(binary)
        (retained / "firmware.elf").write_bytes(elf)
        identity = {"head": "a" * 40, "image": image, "ci_result": "PASS", "ci_exit": 0,
                    "files": {name: hashlib.sha256((retained / name).read_bytes()).hexdigest()
                              for name in ("firmware.bin", "firmware.elf")}}
        (retained / "identity.json").write_text(json.dumps(identity))
        args = SimpleNamespace(candidate_manifest=None, firmware=str(retained / "firmware.bin"),
                               git="a" * 7, image=image[:9])
        return root, retained, identity, args

    def test_audio_cli_requires_same_candidate_microphone_and_fixture_admission(self):
        base = ["run", "--suite", "audio", "--git", IDENTITY["git"], "--image", IDENTITY["image"],
                "--out-dir", str(self.directory / "new-audio")]
        incomplete = ([], ["--firmware", "/fixture/firmware.bin"],
                      ["--firmware", "/fixture/firmware.bin", "--microphone", "Fixture"],
                      ["--firmware", "/fixture/firmware.bin", "--real-v1-off"])
        for extra in incomplete:
            with self.subTest(extra=extra), patch.object(bench, "Runner") as runner, \
                    patch.object(bench, "qualify_candidate") as qualify, redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as caught:
                    bench.main(base + extra)
                self.assertEqual(caught.exception.code, 2)
                runner.assert_not_called()
                qualify.assert_not_called()
        self.assertFalse((self.directory / "new-audio").exists())

    def test_audio_cli_admits_candidate_and_twelve_gb_before_starting_runner(self):
        root = self.directory / "repo"
        replay = root / "tools/v1replay/.build/v1replay"
        replay.parent.mkdir(parents=True)
        replay.write_bytes(b"fixture executable")
        order = []
        def qualify(*_):
            order.append("candidate")
        def start(args, directory):
            order.append("runner")
            self.assertEqual(args.suite, "audio")
            self.assertEqual(args.microphone, "Fixture")
            self.assertTrue(args.real_v1_off)
            return SimpleNamespace(execute=lambda: {"result": "PASS", "restoration": "verified"})
        with patch.object(bench, "ROOT", root), patch.object(bench, "qualify_candidate", side_effect=qualify), \
                patch.object(bench, "require_space") as space, patch.object(bench, "Runner", side_effect=start), \
                patch("behavior_audio.prepare_microphone_recorder", side_effect=lambda: order.append("recorder")), \
                patch.object(bench, "DutLease", return_value=nullcontext()), patch.object(bench.signal, "signal"), \
                redirect_stdout(io.StringIO()):
            result = bench.main(["run", "--suite", "audio", "--git", IDENTITY["git"], "--image", IDENTITY["image"],
                                 "--out-dir", str(self.directory / "audio"), "--firmware", "/fixture/firmware.bin",
                                 "--microphone", "Fixture", "--real-v1-off"])
        self.assertEqual(result, 0)
        self.assertEqual(order, ["candidate", "recorder", "runner"])
        self.assertEqual(space.call_args.args[1], 12_000_000_000)

    def test_warning_cli_requires_candidate_and_fixture_but_never_microphone_preflight(self):
        root = self.directory / "repo"
        replay = root / "tools/v1replay/.build/v1replay"
        replay.parent.mkdir(parents=True)
        replay.write_bytes(b"fixture executable")
        base = ["run", "--suite", "warning", "--git", IDENTITY["git"], "--image", IDENTITY["image"],
                "--out-dir", str(self.directory / "warning")]
        with redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            bench.main(base)
        with patch.object(bench, "ROOT", root), patch.object(bench, "qualify_candidate") as candidate, \
                patch.object(bench, "require_space") as space, patch.object(bench, "Runner") as runner, \
                patch("behavior_audio.prepare_microphone_recorder") as microphone, \
                patch.object(bench, "DutLease", return_value=nullcontext()), patch.object(bench.signal, "signal"), \
                redirect_stdout(io.StringIO()):
            runner.return_value.execute.return_value = {"result": "PASS", "restoration": "verified"}
            self.assertEqual(bench.main(base + ["--firmware", "/fixture/firmware.bin", "--real-v1-off"]), 0)
            candidate.assert_called_once()
            microphone.assert_not_called()
            self.assertEqual(space.call_args.args[1], 5_000_000_000)

    def test_exact_candidate_is_separate_from_dirty_host_tool_identity(self):
        root, _, identity, args = self.candidate()
        with patch.object(bench, "ROOT", root), patch.object(bench.subprocess, "check_output", side_effect=["b" * 40, b" M scripts/behavior_bench.py"]):
            receipt = bench.qualify_candidate(args, self.directory)
        self.assertEqual(receipt["candidate"], identity)
        self.assertEqual(receipt["host_head"], "b" * 40)
        self.assertTrue(receipt["host_worktree_dirty"])
        self.assertIn("scripts/usb_profiles.py", receipt["host_files"])

    def test_wrong_ci_artifact_or_embedded_image_is_rejected_before_device(self):
        for failure in ("ci", "artifact", "descriptor", "git", "image"):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as subdirectory:
                self.directory = Path(subdirectory)
                root, retained, identity, args = self.candidate()
                if failure == "ci":
                    (root / ".artifacts/ci-gate-passed.sha").write_text("b" * 40)
                elif failure == "artifact":
                    (retained / "firmware.bin").write_bytes(b"corrupt")
                elif failure == "descriptor":
                    binary = bytearray((retained / "firmware.bin").read_bytes())
                    binary[176] ^= 1
                    (retained / "firmware.bin").write_bytes(binary)
                    identity["files"]["firmware.bin"] = hashlib.sha256(binary).hexdigest()
                    (retained / "identity.json").write_text(json.dumps(identity))
                elif failure == "git":
                    args.git = "b" * 7
                else:
                    args.image = "b" * 9
                with patch.object(bench, "ROOT", root), patch.object(bench, "LoggedLink") as link:
                    with self.assertRaises(bench.Incomplete):
                        bench.qualify_candidate(args, self.directory)
                    link.assert_not_called()

    def test_shared_dut_lease_excludes_other_runs_and_does_not_inherit_radio_fd(self):
        from run_window import V1_RADIO_LEASE_FD_ENV
        path = self.directory / "dut.lock"
        with patch.dict(os.environ, {V1_RADIO_LEASE_FD_ENV: "invalid-radio-descriptor"}):
            with bench.DutLease(path):
                with self.assertRaises(bench.Incomplete):
                    with bench.DutLease(path):
                        self.fail("second independent run acquired the DUT")
            self.assertEqual(os.environ[V1_RADIO_LEASE_FD_ENV], "invalid-radio-descriptor")
            with bench.DutLease(path):
                pass

    def test_cycle_completion_requires_full_unique_checks_and_verified_restore(self):
        for failure in (None, "missing", "restore", "fail", "interrupt"):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as directory:
                path = Path(directory).resolve()
                calls = []
                def factory(args, child):
                    calls.append((child.name, args.campaign_baseline))
                    number = len(calls)
                    def execute():
                        if number == 2 and failure == "interrupt":
                            raise KeyboardInterrupt("fixture host stopped")
                        checks = [{"check": name, "result": "PASS"} for name in bench.required_checks("profiles")]
                        if number == 2 and failure == "missing":
                            checks.pop()
                        return {"result": "FAIL" if number == 2 and failure == "fail" else "PASS", "scope": "profiles",
                                "restoration": "unverified" if number == 2 and failure == "restore" else "verified", "checks": checks}
                    return SimpleNamespace(execute=execute)
                args = SimpleNamespace(suite="profiles", cycles=3)
                result = bench.run_cycles(args, path, factory)
                self.assertEqual(result["result"], "PASS" if failure is None else "FAIL" if failure == "fail" else "INCOMPLETE")
                self.assertEqual(len(calls), 3 if failure is None else 2)
                self.assertIsNone(calls[0][1])
                self.assertEqual(calls[1][1], str(path / "cycle-0001"))
                manifest = json.loads((path / "campaign.json").read_text())
                self.assertEqual(manifest["active_cycle"], None if failure is None else "cycle-0002")
                self.assertFalse(result["full_hardware_coverage"])

    def test_campaign_recovery_only_targets_interrupted_cycle_and_preserves_original_verdict(self):
        (self.directory / "cycle-0002").mkdir()
        original = {"schema": 1, "requested_cycles": 3, "active_cycle": "cycle-0002", "result": "INCOMPLETE"}
        bench.atomic_json(self.directory / "campaign.json", original)
        bench.atomic_json(self.directory / "result.json", original)
        factory = Mock(return_value=SimpleNamespace(execute=lambda: {"result": "PASS", "restoration": "verified"}))
        bench.restore_campaign(SimpleNamespace(command="restore"), self.directory, factory)
        self.assertEqual(factory.call_args.args[1], self.directory / "cycle-0002")
        self.assertEqual(json.loads((self.directory / "result.json").read_text()), original)
        self.assertEqual(json.loads((self.directory / "campaign.json").read_text()), original)


class WarningNativeFrameTests(unittest.TestCase):
    def frames(self):
        pts = [1. + index * .005 for index in range(11)]
        host = [1_000_000_000 + index * 5_000_000 for index in range(11)]
        return SimpleNamespace(host=host, pts=pts, video=Path("fixture.mov"),
                               rows=[{"frame_seq": index + 100, "source_pts_value": index * 150}
                                     for index in range(11)],
                               decoder=Mock(side_effect=lambda _path, when: (when, "original pixels")))

    def test_every_original_native_frame_is_retained_once_with_exact_identity(self):
        frames = self.frames()
        result = bench._warning_native_frames(frames, 1_025_000_000)
        self.assertEqual([row["frame_seq"] for row in result], list(range(100, 111)))
        self.assertEqual([row["host_capture_ns"] for row in result], frames.host)
        self.assertEqual([row["video_pts_seconds"] for row in result], frames.pts)
        self.assertEqual([row["source_frame"] for row in result], frames.rows)
        self.assertEqual(frames.decoder.call_count, 11)

    def test_missing_duplicate_out_of_order_or_wrong_decoded_frame_is_rejected(self):
        for change in (lambda frames: frames.rows[5].update(frame_seq=106),
                       lambda frames: frames.rows[5].update(frame_seq=104),
                       lambda frames: frames.host.__setitem__(5, frames.host[4]),
                       lambda frames: frames.host.__setitem__(6, frames.host[4]),
                       lambda frames: frames.decoder.configure_mock(
                           side_effect=lambda _path, when: (when + .005, "wrong frame")),
                       lambda frames: frames.decoder.configure_mock(
                           side_effect=lambda _path, when: (float("nan"), "invalid timestamp"))):
            with self.subTest(change=change):
                frames = self.frames()
                change(frames)
                with self.assertRaises(bench.CaptureError):
                    bench._warning_native_frames(frames, 1_025_000_000)
        with self.assertRaises(bench.CaptureError):
            bench._warning_native_frames(self.frames(), 1_025_000_001)


class WarningEvidenceTests(unittest.TestCase):
    def setUp(self):
        from test_behavior_audio_runner import fixture
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        case = {"label": "warning", "phrase": "k", "band": "K", "frequency": 24150, "speaker": 0, "detector": 0}
        base, proof, _, identity = fixture(self.directory, case)
        destination = self.directory / "warning"
        destination.mkdir()
        for name in ("replay_scenario.json", "behavior_replay_result.json", "launch-intent.json"):
            (destination / name).write_bytes((base / "replay" / name).read_bytes())
        replay_path = destination / "behavior_replay_result.json"
        replay = json.loads(replay_path.read_text())
        for row in replay["events"]:
            for key in ("hostMonotonicNs", "requestedHostMonotonicNs"):
                if row.get(key, 0) >= 107_000_000_000:
                    row[key] += 4_000_000_000
        bench.atomic_json(replay_path, replay)
        bench.atomic_json(destination / "warning-input.json", bench.warning_scenario())
        command = json.loads((destination / "launch-intent.json").read_text())
        command["command"].append("--quiet-after-complete")
        bench.atomic_json(destination / "launch-intent.json", command)
        target = f"replay-target-{proof['normal_before']['boot']}.json"
        bench.atomic_json(self.directory / target, proof["target_capture"])
        self.proof = {key: deepcopy(proof[key]) for key in ("normal_before", "normal_after", "idle", "tail", "admitted_target")}
        self.proof.update(proof["configured"], target_file=target)
        for key in ("release_before_ns", "release_after_ns"):
            self.proof["idle"][key] += 4_000_000_000
        for key in ("clear_accepted_ns", "idle_accepted_ns"):
            self.proof["tail"][key] += 4_000_000_000
        self.proof["tail"]["observed_until_ns"] = self.proof["tail"]["idle_accepted_ns"] + 30_100_000_000
        self.plan = {"suite": "warning", "capture_completed": True, "identity": identity, "warning": self.proof}
        self.resolved_band("k")

    def resolved_band(self, band):
        # Authored input uses "K"; the shipping Swift resolved evidence uses
        # AlertBand.name ("k") in both samples and expected delivery rows.
        path = self.directory / "warning/replay_scenario.json"
        resolved = json.loads(path.read_text())
        for row in resolved["samples"]:
            row["alerts"][0]["band"] = band
        bench.atomic_json(path, resolved)
        raw = path.read_bytes()
        sha = hashlib.sha256(raw).hexdigest()
        replay_path = self.directory / "warning/behavior_replay_result.json"
        replay = json.loads(replay_path.read_text())
        for row in replay["events"]:
            if row["state"] == "scenario_resolved":
                row.update(sha256=sha, byteCount=len(raw))
            elif row["state"] == "configured":
                row["scenarioSha256"] = sha
            elif row["state"] == "stimulus_requested":
                row["expected"]["alerts"][0]["band"] = band
        bench.atomic_json(replay_path, replay)
        self.seal()

    def seal(self):
        names = ["warning/warning-input.json", "warning/replay_scenario.json", "warning/behavior_replay_result.json",
                 "warning/launch-intent.json", self.proof["target_file"]]
        self.proof["artifacts"] = {name: hashlib.sha256((self.directory / name).read_bytes()).hexdigest() for name in names}

    def test_complete_warning_evidence_pins_first_zero_delay_and_quiet_tail(self):
        proof = bench.warning_evidence(self.directory, self.plan)
        self.assertEqual(proof["expected_onset_ns"], 120_100_000_000)
        self.assertGreater(proof["tail"]["observed_until_ns"], proof["expected_onset_ns"] + 12_500_000_000)

    def test_authored_uppercase_band_is_not_the_shipping_resolved_schema(self):
        self.resolved_band("K")
        with self.assertRaisesRegex(bench.Incomplete, "Warning resolved stimulus differs"):
            bench.warning_evidence(self.directory, self.plan)

    def test_incomplete_wrong_boot_hash_and_short_silence_are_rejected(self):
        original = deepcopy(self.plan)
        for mutation in (lambda: self.plan.update(capture_completed=False),
                         lambda: self.proof["normal_after"].update(boot=99),
                         lambda: self.proof["admitted_target"].update(session_generation=99),
                         lambda: self.proof["idle"].update(release_before_ns=130_000_000_000),
                         lambda: self.proof["tail"].update(observed_until_ns=120_000_000_000),
                         lambda: self.proof["artifacts"].update({"warning/warning-input.json": "wrong"})):
            self.plan = deepcopy(original)
            self.proof = self.plan["warning"]
            mutation()
            with self.assertRaises(bench.Incomplete):
                bench.warning_evidence(self.directory, self.plan)

    def test_accepted_packet_or_disconnect_during_warning_silence_is_rejected(self):
        path = self.directory / "warning/behavior_replay_result.json"
        original = json.loads(path.read_text())
        for row in ({"state": "notification_accepted", "characteristic": "B2CE", "payloadHex": "", "hostMonotonicNs": 125_000_000_000},
                    {"state": "session_transport", "active": False, "observerHostMonotonicNs": 125_000_000_000}):
            replay = deepcopy(original)
            replay["events"].append(row)
            bench.atomic_json(path, replay)
            self.seal()
            with self.assertRaises((bench.Incomplete, bench.CaptureError)):
                bench.warning_evidence(self.directory, self.plan)


class AnalysisTests(unittest.TestCase):
    def test_cli_refuses_partial_acceptance_and_campaign_reanalysis(self):
        for kind in ("acceptance", "profiles", "audio", "campaign"):
            with self.subTest(kind=kind), tempfile.TemporaryDirectory() as directory:
                path = Path(directory).resolve()
                filename = "campaign.json" if kind == "campaign" else "observations.json"
                (path / filename).write_text(json.dumps({"suite": kind}))
                with patch.object(bench, "analyze") as analyze:
                    with self.assertRaises(bench.Incomplete):
                        bench.main(["analyze", "--out-dir", str(path)])
                    analyze.assert_not_called()

    def test_cli_camera_evidence_error_exits_incomplete_without_masking_other_errors(self):
        import behavior_capture
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory).resolve()
            (path / "observations.json").write_text(json.dumps(observation_plan()))
            argv = ["behavior_bench.py", "analyze", "--out-dir", str(path)]
            for error in (behavior_capture.CaptureError("fixture camera evidence missing"),
                          RuntimeError("fixture unrelated programming error")):
                output = io.StringIO()
                with self.subTest(error=type(error).__name__), patch.object(sys, "argv", argv), \
                        patch.object(behavior_capture, "RecordedFrames", side_effect=error), redirect_stdout(output):
                    if isinstance(error, behavior_capture.CaptureError):
                        with self.assertRaises(SystemExit) as caught:
                            runpy.run_path(str(bench.ROOT / "scripts/behavior_bench.py"), run_name="__main__")
                        self.assertEqual(caught.exception.code, 2)
                        self.assertIn("INCOMPLETE", output.getvalue())
                    else:
                        with self.assertRaisesRegex(RuntimeError, "unrelated programming error"):
                            runpy.run_path(str(bench.ROOT / "scripts/behavior_bench.py"), run_name="__main__")

    def test_declared_suite_requires_all_unique_observations_and_timing(self):
        for suite in ("all", "colors", "clearance"):
            bench.validate_plan(observation_plan(suite))
        mutations = (
            lambda p: p["previews"].pop(),
            lambda p: p["previews"].append(p["previews"][0]),
            lambda p: p["previews"].__setitem__(1, p["previews"][0]),
            lambda p: p["previews"][0].update(ended_ns=2_000_000_000),
            lambda p: p["previews"][0].update(request_after_ns=1_600_000_001),
            lambda p: p["replays"].pop(),
            lambda p: p["replays"].append(p["replays"][0]),
            lambda p: p["replays"][1].update(tails=[{}]),
            lambda p: p["replays"][0].update(result_file="../other.json"),
        )
        for mutation in mutations:
            plan = observation_plan()
            mutation(plan)
            with self.assertRaises(bench.Incomplete):
                bench.validate_plan(plan)

    def test_actual_analysis_retains_first_failure_when_later_decoding_stops(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / "observations.json").write_text(json.dumps(observation_plan("colors")))

            class Frames:
                registration = {}

                def __init__(self, _path):
                    self.calls = 0

                def decoded_samples(self, *args, **kwargs):
                    self.calls += 1
                    if self.calls > 4:
                        raise bench.Incomplete("fixture later decode stopped")
                    return []

            checks = []
            with patch.object(bench, "RecordedFrames", Frames), patch.object(bench, "progress", lambda _: None), \
                    patch.object(bench, "evaluate_palette", return_value={"result": "FAIL", "reason": "fixture wrong color"}):
                with self.assertRaises(bench.Incomplete):
                    bench.analyze(path, checks=checks)
            self.assertEqual(len(checks), 1)
            self.assertEqual(checks[0]["result"], "FAIL")
            saved = json.loads((path / "visual-result.json").read_text())
            self.assertEqual(saved["result"], "FAIL")
            self.assertFalse(saved["analysis_completed"])


if __name__ == "__main__":
    unittest.main()
