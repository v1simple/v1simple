"""Run the real host coordinator against modeled transports, never a DUT.

These checks exercise admission, durable intent, and recovery after independent
camera/browser/replay failures. Pixel and firmware behavior need their own tests.
"""
from contextlib import ExitStack, redirect_stdout
from copy import deepcopy
import json
import io
from pathlib import Path
import runpy
import sys
import tempfile
from types import MethodType, SimpleNamespace
import unittest
from unittest.mock import Mock, patch

import behavior_bench as bench
from test_behavior_state import snapshot, profiles, USB_IDENTITY


IDENTITY = {"git": "123abcd", "image": "abcdef123", "mode": "normal", "boot": 1,
            "slot": 0, "persist": 0, "enabled": True, "busy": False}


class Fixture:
    def __init__(self, directory, failures=()):
        self.directory = directory
        self.failures = set(failures)
        self.events = []
        self.config = snapshot()
        self.initial = deepcopy(self.config)
        self.status_value = deepcopy(IDENTITY)
        self.runner = None
        owner = self

        class Http(bench.HttpDevice):
            def request(self, endpoint, fields=None):
                if fields is not None:
                    if endpoint == "/api/display/settings":
                        owner.events.append("restore_write")
                        if "restore" in owner.failures:
                            raise OSError("fixture restore response lost")
                        owner.config["display"].update(fields)
                    elif endpoint != "/api/display/preview/clear":
                        raise AssertionError("Unplanned fixture write: " + endpoint)
                    return {"success": True}
                from behavior_state import ENDPOINTS
                key = next(key for key, value in ENDPOINTS.items() if value == endpoint)
                return deepcopy(owner.config[key])

            def snapshot(self):
                owner.events.append("snapshot")
                if "snapshot" in owner.failures:
                    raise bench.Incomplete("fixture partial baseline")
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
                pass

            def status(self):
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
                return {}, profiles()

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

    def execute(self, *, command="run", analysis=None, base_url="http://localhost:18765", ssid="Fixture"):
        args = SimpleNamespace(command=command, base_url=base_url, port=None,
                               git=IDENTITY["git"], image=IDENTITY["image"], suite="colors",
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
            if checks is None:
                checks = []
            checks.extend([{"result": "PASS", "check": "modeled_pixels"}] if analysis is None else analysis)
            if "analysis_late_failure" in self.failures:
                raise bench.Incomplete("fixture later capture decode unavailable")
            return checks

        with ExitStack() as stack:
            for name, value in (("HttpDevice", self.Http), ("LoggedLink", self.Link),
                                ("CaptureSession", self.Camera), ("BrowserDriver", self.Browser),
                                ("atomic_json", atomic), ("progress", lambda _: None)):
                stack.enter_context(patch.object(bench, name, value))
            stack.enter_context(patch.object(bench.usb, "Device", self.Device))
            stack.enter_context(patch.object(bench.RecoveryJournal, "save", save))
            stack.enter_context(patch.object(bench.Runner, "wait", lambda *_: None))
            stack.enter_context(patch.object(bench, "analyze", side_effect=analyze))
            self.runner = bench.Runner(args, self.directory)
            self.runner.colors = MethodType(lambda runner: self.colors(runner), self.runner)
            return self.runner.execute()


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


class AnalysisTests(unittest.TestCase):
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
