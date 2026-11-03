#!/usr/bin/env python3
"""Unattended, preserving display/settings qualification on installed firmware.

No flash, factory reset, credential change or detector Apply is performed.
Run artifacts must be private and outside every Git checkout. Use `restore`
after a host interruption; `analyze` reads a finished capture without a DUT.
"""
from __future__ import annotations

import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts/bench"))
import usb_profiles as usb
from behavior_browser import BrowserDriver, canonical_display
from behavior_capture import CaptureError, CaptureSession, ReplaySession, RecordedFrames, verified_replay_delivery
from behavior_state import (BehaviorFailure, HttpDevice, Incomplete, RecoveryJournal,
                            atomic_json, differences, result_status, semantic_snapshot, verify_identity)
from behavior_visual import (observe_frame, evaluate_palette, evaluate_clearance,
                             evaluate_card_clearance, evaluate_takeover)
from run_window import try_join_maintenance_wifi

SENTINELS = {"red": 0xF800, "green": 0x07E0, "blue": 0x001F,
             "cyan": 0x07FF, "magenta": 0xF81F, "yellow": 0xFFE0}
BARS_A = ["red", "green", "blue", "cyan", "magenta", "yellow"]
BARS_B = ["yellow", "red", "green", "blue", "cyan", "magenta"]
PREVIEW_SECONDS = 39.0  # Stop before the muted step at 42 seconds.
DEFAULT_BASE_URL = "http://192.168.35.5"
DEFAULT_SSID = "V1-Simple"


def progress(message: str) -> None:
    print("[behavior] " + message, flush=True)


def private_output(raw: str, *, existing: bool = False) -> Path:
    path = Path(raw).expanduser().absolute()
    if path.resolve() != path:
        raise Incomplete("Output path must not contain symlinks")
    if any((parent / ".git").exists() for parent in (path, *path.parents)):
        raise Incomplete("Output must be outside Git checkouts; it contains private settings")
    if existing:
        if not path.is_dir():
            raise Incomplete("Existing run directory is required")
    else:
        path.mkdir(parents=True, exist_ok=False, mode=0o700)
    os.chmod(path, 0o700)
    return path


def require_space(path: Path, minimum_bytes: int) -> None:
    if shutil.disk_usage(path).free < minimum_bytes:
        raise Incomplete(f"Insufficient recording space; need {minimum_bytes // 1_000_000_000} GB free")


def trial_patch(color: str, bars: list[str], *, band_mode: bool = False) -> dict:
    return {"brightness": 200, "freqUseBandColor": band_mode,
            "freq": SENTINELS[color], "bandX": SENTINELS["red"],
            "bandK": SENTINELS["green"], "bandKa": SENTINELS["blue"],
            "hideBatteryIcon": False, "hideProfileIndicator": False, "hideBleIcon": False,
            **{f"bar{index}": SENTINELS[value] for index, value in enumerate(bars, 1)}}


class LoggedLink(usb.SerialLink):
    """Exactly one serial owner; retain unsolicited boot/alert logs as well."""
    def __init__(self, port, output: Path, identity=None):
        super().__init__(port)
        self.identity = tuple(identity) if identity is not None else None
        self.log = output.open("xb")

    def read(self, count):
        raw = super().read(count)
        if raw:
            self.log.write(json.dumps({"host_monotonic_ns": time.monotonic_ns(),
                                      "bytes_hex": raw.hex()}).encode() + b"\n")
            self.log.flush()
        return raw

    def finish(self):
        self.close()
        self.log.close()


class Runner:
    def __init__(self, args, directory: Path):
        self.args, self.directory = args, directory
        self.journal = RecoveryJournal.load(directory) if args.command == "restore" else None
        saved_connection = self.journal.value.get("connection", {}) if self.journal else {}
        self.connection = {
            "base_url": args.base_url or saved_connection.get("base_url", DEFAULT_BASE_URL),
            "ssid": args.ssid or saved_connection.get("ssid", DEFAULT_SSID),
        }
        self.http = HttpDevice(self.connection["base_url"])
        self.connection["base_url"] = self.http.base_url
        identity = self.journal.value["usb_identity"] if self.journal else None
        stamp = time.time_ns()
        self.link = LoggedLink(args.port, directory / f"usb-{stamp}.ndjson", identity)
        self.device = usb.Device(usb.Client(self.link))
        self.expected = (self.journal.value["identity"] if self.journal else
                         {"git": args.git[:7], "image": args.image})
        self.camera = self.replay = self.browser = None
        self.capture_finished = False
        self.checks = []
        self.plan = {"schema": 1, "identity": self.expected, "suite": args.suite,
                     "previews": [], "replays": [], "capture_completed": False,
                     "timing_tolerance_s": args.tolerance,
                     "host_source": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT,
                                                             text=True).strip()}
        self.plan["host_tool_sha256"] = {
            str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in [Path(__file__), *(ROOT / "scripts/bench").glob("behavior_*.py")]}
        self._last_progress = 0.0

    def status(self):
        actual = self.device.status()
        verify_identity(actual, self.expected)
        return actual

    def mode(self, target: str):
        original = self.status()
        actual = self.device.mode(target, original)
        verify_identity(actual, self.expected)
        if target == "maintenance":
            deadline = time.monotonic() + 60
            last = None
            while time.monotonic() < deadline:
                try:
                    self.http.display()
                    return actual
                except Exception as exc:
                    last = exc
                    try_join_maintenance_wifi(ssid=self.connection["ssid"], timeout_s=min(10, deadline-time.monotonic()))
                    time.sleep(.25)
            raise Incomplete("Maintenance website could not be reached") from last
        return actual

    def pump(self):
        self.link.read(4096)
        if self.camera and not self.capture_finished:
            problem = self.camera.health_problem()
            if problem:
                raise Incomplete("Camera capture failed: " + problem)
            require_space(self.directory, 1_000_000_000)
        now = time.monotonic()
        if now - self._last_progress >= 20:
            progress("recording; device operations remain bounded")
            self._last_progress = now

    def wait(self, seconds: float):
        until = time.monotonic() + seconds
        while time.monotonic() < until:
            self.pump()
            time.sleep(.05)

    def browser_open(self):
        self.browser = BrowserDriver(self.connection["base_url"], self.directory / "browser",
                                     headless=not self.args.headed)
        self.browser.__enter__()

    def save_display(self, patch: dict, label: str):
        current = self.http.display()
        target = canonical_display({**current, **patch})
        if target == current:
            return target
        self.journal.validate_display_change(current, patch)
        self.journal.display_intent(patch)
        result = self.browser.save_display(patch, current)
        atomic_json(self.directory / f"{label}-browser.json", result)
        if result["result"] == "FAIL":
            raise BehaviorFailure("Website Save failed verified readback: " + label)
        if result["result"] != "PASS":
            raise Incomplete("Website Save did not complete unambiguously: " + label)
        if self.http.display() != target:
            raise BehaviorFailure("Independent display readback differs from the submitted setting")
        return target

    def preview(self, label: str, target: dict, bars: list[str], band_mode: bool):
        before_status = self.status()
        if before_status["mode"] != "maintenance":
            raise Incomplete("Preview requires confirmed maintenance mode")
        before, after = self.http.preview_start()
        if after - before > 500_000_000:
            raise Incomplete("Preview start response uncertainty exceeds the observation budget")
        self.wait(PREVIEW_SECONDS)
        ended = time.monotonic_ns()
        self.http.request("/api/display/preview/clear", {})
        after_status = self.status()
        if any(after_status[key] != before_status[key]
               for key in ("git", "image", "mode", "boot", "slot", "persist", "enabled")):
            raise BehaviorFailure("DUT mode/boot/profile changed during preview")
        colors = ({"10.525": "red", "24.150": "green", "33.800": "blue", "34.700": "blue"}
                  if band_mode else {frequency: next(key for key, value in SENTINELS.items() if value == target["freq"])
                                     for frequency in ("10.525", "24.150", "33.800", "34.700")})
        self.plan["previews"].append({"label": label, "request_before_ns": before,
                                      "request_after_ns": after, "ended_ns": ended,
                                      "expected_frequency_colors": colors, "expected_bars": bars})
        atomic_json(self.directory / "observations.json", self.plan)

    def colors(self):
        for name, color, bars, band in (("custom_a", "magenta", BARS_A, False),
                                         ("custom_b", "cyan", BARS_B, False),
                                         ("band_colors", "magenta", BARS_B, True)):
            progress("checking saved colors: " + name)
            target = self.save_display(trial_patch(color, bars, band_mode=band), name)
            self.preview(name + "_saved", target, bars, band)
            self.mode("normal")
            self.mode("maintenance")
            if self.http.display() != target:
                raise BehaviorFailure("Display setting did not survive normal reboot: " + name)
            self.preview(name + "_rebooted", target, bars, band)

    def run_replay(self, name: str, scenario: str, persist: int):
        before = self.status()
        if before["mode"] != "normal" or before["persist"] != persist:
            raise Incomplete("Normal firmware does not report the selected persistence")
        self.replay = ReplaySession(ROOT / "tools/v1replay/.build/v1replay",
                                    self.directory / name, scenario)
        self.replay.start()
        completed = False
        tails = []
        try:
            self.replay.wait_complete(timeout_s=150, pump=self.pump)
            tails.append(self.replay.hold_quiet(6, pump=self.pump))
            if scenario == "clearance":
                self.replay.restart()
                self.replay.wait_complete(timeout_s=30, pump=self.pump)
                tails.append(self.replay.hold_quiet(6, pump=self.pump))
            completed = True
        finally:
            result = self.replay.finish(completed)
            self.replay = None
        if not result["lifecycle_completed"]:
            raise Incomplete("Replay lifecycle or graceful termination was not confirmed")
        after = self.status()
        if after["boot"] != before["boot"] or after["mode"] != "normal":
            raise BehaviorFailure("Unexpected reboot during the alert replay")
        if any(after[key] != before[key] for key in ("persist", "slot", "enabled")):
            raise Incomplete("Replay selected a different slot or persistence policy")
        record = {"name": name, "scenario": scenario, "persistence_s": persist, "tails": tails,
                  "result_file": name + "/behavior_replay_result.json"}
        self.plan["replays"].append(record)
        atomic_json(self.directory / "observations.json", self.plan)

    def clearance(self):
        # Conspicuous known optics; the recovery record owns every changed field.
        patch = trial_patch("red", BARS_A)
        patch.update(bogey=0xF800, arrowFront=0xF800, arrowSide=0xF800, arrowRear=0xF800,
                     muted=0x7BEF, persisted=0x7BEF)
        self.save_display(patch, "clearance_palette")
        for persist in (0, 3):
            progress(f"checking alert disappearance with {persist}-second persistence")
            slots = self.http.request("/api/autopush/slots")
            slot = slots["activeSlot"]
            self.journal.validate_persistence_change(slots, slot)
            self.journal.persistence_intent(slot, persist)
            self.http.persistence(slot, slots, persist)
            self.http.request("/api/display/preview/clear", {})
            self.mode("normal")
            self.run_replay(f"persistence-{persist}", "persistence", persist)
            self.run_replay(f"clearance-{persist}", "clearance", persist)
            self.mode("maintenance")

    def restore(self):
        if self.journal is None:
            return "not_needed"
        progress("restoring and checking the original configuration")
        self.mode("maintenance")
        self.http.request("/api/display/preview/clear", {})
        current = self.http.display()
        patch = self.journal.display_restore_patch(current)
        if patch:
            # Recovery uses a narrow saved-field write, independent of browser health.
            try:
                self.http.request("/api/display/settings", patch)
            except Exception:
                pass  # Never repeat an uncertain write; authoritative readback follows.
            expected = {**current, **patch}
            if self.http.display() != expected:
                raise Incomplete("Original display fields could not be restored")
        slots = self.http.request("/api/autopush/slots")
        for slot, value in self.journal.persistence_restore(slots):
            self.http.persistence(slot, slots, value)
            slots = self.http.request("/api/autopush/slots")
        self.http.request("/api/display/preview/clear", {})
        # Durable recovery is observed after a normal boot, not just in RAM.
        self.mode("normal")
        self.mode("maintenance")
        after = self.http.snapshot()
        _, profiles = self.device.backup()
        drift = differences(semantic_snapshot(self.journal.value["baseline"]), semantic_snapshot(after))
        if not usb.same_bundle(self.journal.value["profiles"], profiles):
            drift.append("usb_profiles")
        atomic_json(self.directory / f"restoration-{time.time_ns()}.json", {"snapshot": after, "differences": drift})
        if drift:
            raise Incomplete("Final configuration differs from the saved baseline: " + ", ".join(drift[:12]))
        final = self.mode(self.journal.value["identity"]["mode"])
        self.journal.save(restoration="verified", stage="restored", final_status=final)
        return "verified"

    def execute(self):
        completed, error, restoration = False, None, "not_needed"
        restoration_attempted = False
        initial_status = None
        cleanup_errors = []

        def cleanup(name, action):
            try:
                return action()
            except BaseException as exc:
                cleanup_errors.append(f"{name}: {type(exc).__name__}: {exc}")
                progress(name + " incomplete; continuing independent recovery")
                return None

        def restore_once():
            nonlocal restoration_attempted, restoration, error
            if restoration_attempted:
                if restoration != "verified":
                    restoration = "unverified"
                return
            restoration_attempted = True
            if self.journal:
                try:
                    restoration = self.restore()
                except BaseException as exc:
                    restoration = "unverified"
                    cleanup("recovery record", lambda: self.journal.save(
                        restoration=restoration, restoration_error=str(exc)))
                    error = error or "Restoration could not be verified: " + str(exc)
                    progress("restoration incomplete; saved recovery record retained")
            elif initial_status is not None:
                # Snapshot admission failed before writes. Still restore the
                # mode changed while attempting that read-only admission.
                cleanup("original mode", lambda: self.mode(initial_status["mode"]))

        try:
            current = self.status()
            initial_status = current
            if self.journal:
                restoration_attempted = True
                restoration = self.restore()
                completed = True
                self.checks.append({"result": "PASS", "check": "restoration"})
            else:
                self.mode("maintenance")
                baseline = self.http.snapshot()
                _, profiles = self.device.backup()
                self.journal = RecoveryJournal.create(self.directory, current, baseline, profiles,
                                                      self.link.identity, connection=self.connection)
                self.mode("normal")
                self.wait(2)
                self.camera = CaptureSession(self.directory / "camera")
                self.camera.start()
                self.mode("maintenance")
                self.browser_open()
                if self.args.suite in ("all", "colors"):
                    self.colors()
                if self.args.suite in ("all", "clearance"):
                    self.clearance()
                completed = True
        except BaseException as exc:
            error = f"{type(exc).__name__}: {exc}"
            if isinstance(exc, BehaviorFailure):
                self.checks.append({"result": "FAIL", "check": "device_behavior", "reason": str(exc)})
            progress(error)
        finally:
            if self.replay:
                cleanup("replay shutdown", lambda: self.replay.finish(False))
                self.replay = None
            if self.browser:
                cleanup("browser shutdown", lambda: self.browser.__exit__(None, None, None))
            if self.camera:
                capture = cleanup("camera shutdown", lambda: self.camera.finish(
                    completed, after_recording=restore_once)) or {}
                self.capture_finished = True
                self.plan["capture_completed"] = capture.get("result") == "CAPTURED" and completed
                if capture.get("result") != "CAPTURED":
                    error = error or "Camera evidence did not finalize successfully"
                cleanup("capture record", lambda: atomic_json(self.directory / "observations.json", self.plan))
            # Covers failed admission and a recorder failure before the hook.
            # The callback and fallback share one attempt, never two writes.
            restore_once()
            cleanup("USB shutdown", self.link.finish)
        if cleanup_errors:
            error = error or "Cleanup could not be fully verified"
        if completed and self.args.command == "run" and self.plan["capture_completed"]:
            try:
                analyze(self.directory, tolerance=self.args.tolerance, checks=self.checks)
            except Exception as exc:
                error = error or f"Visual analysis incomplete: {exc}"
        report = {"operation": self.args.command,
                  "result": result_status(self.checks, completed=completed, restoration=restoration, error=error),
                  "candidate": self.expected, "collection_completed": completed,
                  "restoration": restoration, "error": error, "checks": self.checks,
                  "cleanup_errors": cleanup_errors,
                  "firmware_changed": False, "no_device_receipt_latency_claim": True}
        # A successful recovery does not turn an earlier failed or incomplete
        # qualification into a pass. Keep its summary at the original path.
        result_name = "restoration-result" if self.args.command == "restore" else "result"
        atomic_json(self.directory / f"{result_name}-{time.time_ns()}.json", report)
        atomic_json(self.directory / f"{result_name}.json", report)
        return report


def validate_plan(plan: dict) -> None:
    if plan.get("schema") != 1 or plan.get("suite") not in ("all", "colors", "clearance"):
        raise Incomplete("Unsupported observation plan")
    expected_previews = ({name + suffix for name in ("custom_a", "custom_b", "band_colors")
                          for suffix in ("_saved", "_rebooted")}
                         if plan["suite"] in ("all", "colors") else set())
    previews = plan.get("previews", [])
    if (len(previews) != len(expected_previews)
            or {row.get("label") for row in previews} != expected_previews):
        raise Incomplete("The selected suite's complete preview coverage is required")
    for row in previews:
        if (not 0 <= row["request_after_ns"] - row["request_before_ns"] <= 500_000_000
                or row["ended_ns"] - row["request_after_ns"] < 38_000_000_000):
            raise Incomplete("Preview timing does not cover the declared observations")
    expected_replays = ({f"{scenario}-{persist}" for scenario in ("persistence", "clearance")
                         for persist in (0, 3)} if plan["suite"] in ("all", "clearance") else set())
    replays = plan.get("replays", [])
    if (len(replays) != len(expected_replays)
            or {row.get("name") for row in replays} != expected_replays):
        raise Incomplete("The selected suite's complete replay coverage is required")
    for row in replays:
        if (row["scenario"] not in ("persistence", "clearance")
                or row["persistence_s"] not in (0, 3)
                or row["name"] != f"{row['scenario']}-{row['persistence_s']}"
                or row["result_file"] != row["name"] + "/behavior_replay_result.json"
                or len(row["tails"]) != (2 if row["scenario"] == "clearance" else 1)):
            raise Incomplete("Invalid replay coverage record")


def analyze(directory: Path, *, tolerance: float = .35, checks: list[dict] | None = None) -> list[dict]:
    plan = json.loads((directory / "observations.json").read_text())
    validate_plan(plan)
    if plan.get("capture_completed") is not True:
        raise Incomplete("A complete capture is required for analysis")
    if tolerance != plan["timing_tolerance_s"]:
        raise Incomplete("Analysis cannot change the observation tolerance after acquisition")
    frames = RecordedFrames(directory / "camera")
    if checks is None:
        checks = []

    def recorded(check):
        checks.append(check)
        # Preserve earlier observed failures if a later decode is interrupted.
        atomic_json(directory / "visual-result.json", {"checks": checks, "analysis_completed": False,
                    "result": "FAIL" if any(c["result"] == "FAIL" for c in checks) else "INCOMPLETE"})

    def observations(start, end):
        decoded = frames.decoded_samples(int(start), int(end), step_s=.2,
                                        progress=lambda count, total: progress(
                                            f"decoding recorded evidence: {count}/{total}") if count % 20 == 0 else None)
        result = []
        for sample in decoded:
            observation = observe_frame(sample["rgb"], frames.registration)
            result.append({"time_s": sample["host_monotonic_ns"] / 1e9,
                           "video_pts_seconds": sample["video_pts_seconds"], **observation})
        return result

    for preview in plan["previews"]:
        progress("reading original camera frames: " + preview["label"])
        start = preview["request_after_ns"]
        observed = []
        # Stable interior windows of known unmuted steps; identities are decoded
        # from pixels and are independently required by evaluate_palette.
        for low, high in ((.65, 1.45), (10.65, 11.45), (16.65, 17.45), (36.65, 37.45)):
            observed.extend(observations(start + int(low*1e9), start + int(high*1e9)))
        result = evaluate_palette(observed,
                                  expected_frequency_colors=preview["expected_frequency_colors"],
                                  expected_bars=preview["expected_bars"])
        recorded({"check": preview["label"], **result})
        atomic_json(directory / (preview["label"] + "-pixels.json"), observed)

    idle_by_persist = {}
    for item in plan["replays"]:
        progress("reading original camera frames: " + item["name"])
        result = json.loads((directory / item["result_file"]).read_text())
        delivery = verified_replay_delivery(result, (directory / item["result_file"]).parent / "replay_scenario.json")
        atomic_json(directory / (item["name"] + "-delivery.json"), delivery)
        phases = {row["phase"]: row for row in delivery["phase_intervals"]}
        persist = item["persistence_s"]
        if item["scenario"] == "persistence":
            phase = phases["persistence_initial_idle"]
            idle = observations(phase["start_ns"] + 2_000_000_000, phase["start_ns"] + 3_000_000_000)
            idle_by_persist[persist] = idle
            primary_only = phases["persistence_live_preemption"]
            empty = observations(primary_only["start_ns"] + 4_000_000_000,
                                 primary_only["start_ns"] + 5_000_000_000)
            if persist == 3:
                release = phases["persistence_preemption_release"]["start_ns"] / 1e9
                new_alert = primary_only["start_ns"] / 1e9
                takeover = observations(release*1e9, (new_alert+1.3)*1e9)
                check = evaluate_takeover(takeover, new_alert_time_s=new_alert,
                                          release_time_s=release, persistence_s=persist,
                                          tolerance_s=tolerance)
                recorded({"check": item["name"] + "_fresh_takeover", **check})
                atomic_json(directory / (item["name"] + "-takeover-pixels.json"), takeover)
            released = phases["persistence_secondary_release"]["start_ns"] / 1e9
            observed = observations((released - 1.2)*1e9, (released + persist + 1.7)*1e9)
            check = evaluate_card_clearance(observed, empty, active_window=(released-1.0, released-.4),
                                            clear_time_s=released, persistence_s=persist, tolerance_s=tolerance)
            recorded({"check": item["name"] + "_secondary", "anchor": "host_notification_acceptance", **check})
            atomic_json(directory / (item["name"] + "-pixels.json"), {"idle": idle, "empty_card": empty, "samples": observed})
        else:
            if persist not in idle_by_persist:
                raise Incomplete("Missing independent connected-idle control")
            for index, tail in enumerate(item["tails"]):
                skew = tail.get("clear_bundle_skew_ns")
                if type(skew) is not int or not 0 <= skew <= min(tolerance, .25) * 1e9:
                    raise Incomplete("Complete clear bundle exceeds the declared observation allowance")
                clear = tail["clear_accepted_ns"] / 1e9
                observed = observations((clear-1.2)*1e9, (clear+persist+1.7)*1e9)
                check = evaluate_clearance(observed, idle_by_persist[persist],
                                           active_window=(clear-1., clear-.4), clear_time_s=clear,
                                           persistence_s=persist, tolerance_s=tolerance)
                recorded({"check": item["name"] + f"_primary_{index}",
                          "anchor": tail["anchor_kind"], **check})
                atomic_json(directory / (item["name"] + f"-pixels-{index}.json"), observed)
    atomic_json(directory / "visual-result.json", {"checks": checks, "analysis_completed": True,
                "result": "FAIL" if any(c["result"] == "FAIL" for c in checks) else
                          "PASS" if checks and all(c["result"] == "PASS" for c in checks) else "INCOMPLETE"})
    return checks


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("run", "restore", "analyze"))
    parser.add_argument("--out-dir", required=True)
    parser.add_argument("--port")
    parser.add_argument("--git", default="")
    parser.add_argument("--image", default="")
    parser.add_argument("--base-url", help="maintenance origin; restore defaults to the saved run connection")
    parser.add_argument("--ssid", help="maintenance Wi-Fi; restore defaults to the saved run connection")
    parser.add_argument("--suite", choices=("all", "colors", "clearance"), default="all")
    parser.add_argument("--headed", action="store_true")
    parser.add_argument("--tolerance", type=float, default=.35,
                        help="predeclared optical observation allowance, not a device latency guarantee")
    args = parser.parse_args(argv)
    os.umask(0o077)
    if not .1 <= args.tolerance <= .5:
        parser.error("observation tolerance must be 0.1 through 0.5 seconds")
    if args.command == "run" and (not re.fullmatch(r"[0-9a-f]{7,40}", args.git)
                                   or not re.fullmatch(r"[0-9a-f]{9}", args.image)):
        parser.error("run requires a pinned --git and --image identity")
    directory = private_output(args.out_dir, existing=args.command != "run")
    with (directory / ".runner.lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        if args.command == "analyze":
            checks = analyze(directory, tolerance=args.tolerance)
            return 1 if any(c["result"] == "FAIL" for c in checks) else 0 if checks and all(c["result"] == "PASS" for c in checks) else 2
        if args.command == "run":
            # At the fixed 80 Mbps camera profile, the measured clearance leg
            # uses about 3.5 GB including admission. Keep the 1 GB live reserve.
            minimum_gb = {"all": 12, "colors": 7, "clearance": 5}[args.suite]
            require_space(directory, minimum_gb * 1_000_000_000)
            if not (ROOT / "tools/v1replay/.build/v1replay").is_file():
                raise Incomplete("Build the existing v1replay host tool before running")
        def stop(_signal, _frame):
            raise KeyboardInterrupt("Interrupted; restoring owned settings")
        signal.signal(signal.SIGTERM, stop)
        result = Runner(args, directory).execute()
        progress(f"{args.command}: {result['result']}; restoration={result['restoration']}; evidence={directory}")
        return {"PASS": 0, "FAIL": 1, "INCOMPLETE": 2}[result["result"]]


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (Incomplete, CaptureError, OSError, ValueError) as exc:
        progress("INCOMPLETE: " + str(exc))
        raise SystemExit(2)
