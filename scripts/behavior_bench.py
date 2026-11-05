#!/usr/bin/env python3
"""Unattended, preserving behavior qualification on installed firmware.

No flash, factory reset, credential change or detector Apply is performed.
Run artifacts must be private and outside every Git checkout. Use `restore`
after a host interruption; `analyze` reads a finished capture without a DUT.
"""
from __future__ import annotations

import argparse
from bisect import bisect_left, bisect_right
from copy import deepcopy
import fcntl
import hashlib
import importlib.metadata
import json
import os
from pathlib import Path
import re
import secrets
import shutil
import signal
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts/bench"))
import usb_profiles as usb
from behavior_browser import BrowserDriver, canonical_display
from behavior_capture import (CaptureError, CaptureSession, ReplaySession, RecordedFrames, verified_replay_delivery,
                              warning_scenario, accepted_capture_replies, terminal_clear_bundle, is_idle_notification)
from behavior_state import (BehaviorFailure, DeviceHttpError, HttpDevice, Incomplete, RecoveryJournal,
                            atomic_json, differences, result_status, semantic_snapshot, verify_identity)
from behavior_visual import (observe_frame, evaluate_palette, evaluate_clearance,
                             evaluate_card_clearance, evaluate_takeover)
from run_window import try_join_maintenance_wifi, V1RadioLease, ACCOUNT_HOME

SENTINELS = {"red": 0xF800, "green": 0x07E0, "blue": 0x001F,
             "cyan": 0x07FF, "magenta": 0xF81F, "yellow": 0xFFE0}
BARS_A = ["red", "green", "blue", "cyan", "magenta", "yellow"]
BARS_B = ["yellow", "red", "green", "blue", "cyan", "magenta"]
PREVIEW_SECONDS = 39.0  # Stop before the muted step at 42 seconds.
DEFAULT_BASE_URL = "http://192.168.35.5"
DEFAULT_SSID = "V1-Simple"
VISUAL_SUITES = ("all", "colors", "clearance", "acceptance")
AUDIO_SUITES = ("audio", "acceptance")
WARNING_CHECKS = ("warning_idle_controls", "warning_onset", "warning_blink", "warning_expiry")
AUDIO_CHECKS = tuple(name + suffix for name in ("audio_ka", "audio_detector_zero", "audio_x",
                                              "audio_speaker_zero", "audio_recovery")
                     for suffix in ("", "_visual"))


def required_checks(suite: str) -> set[str]:
    checks = set()
    if suite in ("all", "colors", "acceptance"):
        checks.update(name + suffix for name in ("custom_a", "custom_b", "band_colors")
                      for suffix in ("_saved", "_rebooted"))
    if suite in ("all", "clearance", "acceptance"):
        checks.update(f"persistence-{persist}_secondary" for persist in (0, 3))
        checks.add("persistence-3_fresh_takeover")
        checks.update(f"clearance-{persist}_primary_{index}" for persist in (0, 3) for index in (0, 1))
    if suite in ("profiles", "acceptance"):
        checks.update("profiles_" + action for action in ("create", "edit", "collision", "retention", "delete"))
    if suite in AUDIO_SUITES:
        checks.update(AUDIO_CHECKS)
    if suite == "warning":
        checks.update(WARNING_CHECKS)
    return checks


class DutLease(V1RadioLease):
    """Exclude separate behavior runs through USB reopen and maintenance gaps."""
    def __init__(self, path=None):
        super().__init__(path or ACCOUNT_HOME / ".local/state/v1simple/managed-dut.lock", quiet_seconds=0)

    def __enter__(self):
        # The radio lease's inherited descriptor belongs to a different resource.
        from run_window import V1_RADIO_LEASE_FD_ENV
        inherited = os.environ.pop(V1_RADIO_LEASE_FD_ENV, None)
        try:
            try:
                return super().__enter__()
            except RuntimeError as exc:
                raise Incomplete("Managed DUT lease unavailable: " + str(exc)) from exc
        finally:
            if inherited is not None:
                os.environ[V1_RADIO_LEASE_FD_ENV] = inherited


def qualify_candidate(args, directory: Path) -> dict:
    """Bind the retained CI-qualified product separately from the running host tools."""
    path = (Path(args.candidate_manifest) if args.candidate_manifest else
            Path(args.firmware).parent / "identity.json").expanduser().resolve()
    if args.firmware and Path(args.firmware).expanduser().resolve() != path.parent / "firmware.bin":
        raise Incomplete("Selected firmware is not the retained candidate manifest application")
    candidate = json.loads(path.read_text())
    head = candidate.get("head", "")
    if (not re.fullmatch(r"[0-9a-f]{40}", head) or not head.startswith(args.git)
            or candidate.get("ci_result") != "PASS" or candidate.get("ci_exit") != 0
            or (ROOT / ".artifacts/ci-gate-passed.sha").read_text().strip() != head):
        raise Incomplete("Candidate must have the matching successful exact-commit CI receipt")
    files = candidate.get("files", {})
    if not {"firmware.bin", "firmware.elf"} <= files.keys():
        raise Incomplete("Candidate receipt lacks the application and ELF")
    for name, expected in files.items():
        if (Path(name).name != name or name in (".", "..")
                or not isinstance(expected, str) or not re.fullmatch(r"[0-9a-f]{64}", expected)
                or hashlib.sha256((path.parent / name).read_bytes()).hexdigest() != expected):
            raise Incomplete("Candidate artifact identity mismatch")
    image = candidate.get("image")
    firmware = (path.parent / "firmware.bin").read_bytes()
    if (image != files["firmware.elf"] or not image.startswith(args.image)
            or len(firmware) < 208 or firmware[0] != 0xE9
            or firmware[176:208].hex() != image):
        raise Incomplete("Application descriptor, ELF and installed image identity disagree")
    host_head = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    host_dirty = bool(subprocess.check_output(["git", "status", "--porcelain"], cwd=ROOT).strip())
    # Include lazy imports and the shipping adapters used to form expectations,
    # not only the Python modules already imported at admission time.
    tools = [item for folder, suffixes in (("scripts", {".py", ".sh", ".swift", ".txt"}),
                                            ("interface/src", {".js", ".ts", ".svelte"}),
                                            ("tools/v1replay/Sources", {".swift"}),
                                            ("tools/freq_audio/mulaw", {".mul"}))
             for item in (ROOT / folder).rglob("*") if item.is_file() and item.suffix in suffixes]
    for relative in ("tools/v1replay/Package.swift", "interface/package-lock.json", "behavior-bench.sh"):
        if (ROOT / relative).is_file():
            tools.append(ROOT / relative)
    receipt = {"result": "PASS", "candidate": candidate, "manifest_sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
               "host_head": host_head, "host_worktree_dirty": host_dirty,
               "host_files": {str(item.relative_to(ROOT)): hashlib.sha256(item.read_bytes()).hexdigest() for item in sorted(tools)}}
    receipt["python"] = {"version": sys.version, "executable_sha256": hashlib.sha256(Path(sys.executable).read_bytes()).hexdigest(),
                         "distributions": {item.metadata["Name"]: {"version": item.version,
                             "record_sha256": hashlib.sha256((item.read_text("RECORD") or "").encode()).hexdigest()}
                                           for item in importlib.metadata.distributions()
                                           if item.metadata.get("Name")}}
    receipt["host_executables"] = {name: hashlib.sha256(Path(path).read_bytes()).hexdigest()
                                   for name in ("node", "ffmpeg", "ffprobe", "swift")
                                   if (path := shutil.which(name)) is not None}
    replay = ROOT / "tools/v1replay/.build/v1replay"
    if replay.is_file():
        receipt["replay_executable_sha256"] = hashlib.sha256(replay.read_bytes()).hexdigest()
    atomic_json(directory / "candidate-admission.json", receipt)
    return receipt


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

    def initial_status(self):
        # Opening USB can restart a slow storage boot. Only this initial,
        # read-only admission shares the existing controlled-boot budget.
        client = self.device.client
        deadline = client.clock() + self.device.mode_timeout
        last = None
        while client.clock() < deadline:
            try:
                actual = self.device.status(timeout=min(2., deadline - client.clock()),
                                            attempts=1, deadline=deadline)
            except usb.TransportError as exc:
                last = exc
            else:
                verify_identity(actual, self.expected)
                if client.clock() >= deadline:
                    break
                return actual
            client.sleep(min(.1, max(0., deadline - client.clock())))
        raise usb.TransportError("Initial USB status was not confirmed within the startup deadline") from last

    def mode(self, target: str):
        original = self.status()
        actual = self.device.mode(target, original)
        verify_identity(actual, self.expected)
        if target == "maintenance":
            deadline = time.monotonic() + 60
            last = None
            while time.monotonic() < deadline:
                try:
                    self.http.display(timeout_s=deadline-time.monotonic())
                    if time.monotonic() >= deadline:
                        break
                    return actual
                except Exception as exc:
                    last = exc
                    remaining = deadline-time.monotonic()
                    if remaining <= 0:
                        break
                    try_join_maintenance_wifi(ssid=self.connection["ssid"], timeout_s=min(30, remaining))
                    time.sleep(min(.25, max(0., deadline-time.monotonic())))
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
            self.observe_replay_target()
            self.mode("normal")
            self.run_replay(f"clearance-{persist}", "clearance", persist)
            self.observe_replay_target()

    def observe_replay_target(self):
        """Bind natural last-connected metadata to this controlled replay boot."""
        normal = self.status()
        if normal["mode"] != "normal":
            raise Incomplete("Replay target observation requires its normal boot")
        self.mode("maintenance")
        captured = self.http.request("/api/v1/current")
        atomic_json(self.directory / f"replay-target-{normal['boot']}.json", captured)
        self.journal.admit_replay_target(captured, normal["boot"])

    def warning(self):
        """Exercise the zero-volume warning without acquiring any microphone."""
        if not self.args.real_v1_off:
            raise Incomplete("Warning replay requires the real detector to be off")
        self.save_display({"brightness": 200, "hideBatteryIcon": False,
                           "hideProfileIndicator": False, "hideBleIcon": False}, "warning-display")
        audio = self.http.request("/api/audio/settings")
        patch = {"voiceVolume": 0, "speedMuteEnabled": False, "stealthEnabled": False}
        self.journal.audio_intent(audio, patch)
        self.recovery_write("warning-audio", "/api/audio/settings", patch)
        configured_audio = self.http.request("/api/audio/settings")
        if configured_audio != {**audio, **patch}:
            raise Incomplete("Warning fixture audio configuration did not read back")
        slots = self.http.request("/api/autopush/slots")
        self.journal.autopush_intent(slots, False)
        if slots["enabled"]:
            self.recovery_write("warning-autopush", "/api/autopush/activate",
                                {"slot": slots["activeSlot"], "enable": False})
            actual = self.http.request("/api/autopush/slots")
            if actual != {**slots, "enabled": False}:
                raise Incomplete("Warning fixture AutoPush configuration did not read back")
            slots = actual
        slot = slots["activeSlot"]
        self.journal.validate_persistence_change(slots, slot)
        self.journal.persistence_intent(slot, 0)
        self.http.persistence(slot, slots, 0)
        configured_slots = self.http.request("/api/autopush/slots")
        self.http.request("/api/display/preview/clear", {})
        before = self.mode("normal")
        if before["persist"] != 0 or before["enabled"] is not False:
            raise Incomplete("Warning fixture requires zero persistence and disabled AutoPush")
        self.replay = ReplaySession(ROOT / "tools/v1replay/.build/v1replay", self.directory / "warning",
                                    "warning", authored=warning_scenario(), detector_volume=0)
        self.replay.start()
        completed = False
        try:
            idle = self.replay.release_after_idle(7, pump=self.pump)
            self.replay.wait_complete(timeout_s=40, pump=self.pump)
            tail = self.replay.hold_quiet(30, pump=self.pump)
            completed = True
        finally:
            result = self.replay.finish(completed)
            self.replay = None
        if not result["lifecycle_completed"]:
            raise Incomplete("Warning replay did not close completely")
        after = self.status()
        if before != after:
            raise Incomplete("Warning replay changed DUT boot or runtime identity")
        self.observe_replay_target()
        target_path = self.directory / f"replay-target-{before['boot']}.json"
        if (self.http.request("/api/audio/settings") != configured_audio
                or self.http.request("/api/autopush/slots") != configured_slots):
            raise Incomplete("Warning fixture configuration was not retained")
        paths = ["warning/warning-input.json", "warning/replay_scenario.json",
                 "warning/behavior_replay_result.json", "warning/launch-intent.json", target_path.name]
        self.plan["warning"] = {"normal_before": before, "normal_after": after, "idle": idle, "tail": tail,
            "audio": configured_audio, "slots": configured_slots, "target_file": target_path.name,
            "admitted_target": self.journal.value["replay_target"],
            "artifacts": {name: hashlib.sha256((self.directory / name).read_bytes()).hexdigest() for name in paths}}
        atomic_json(self.directory / "observations.json", self.plan)

    def full_profiles(self, snapshot):
        return {row["name"]: self.http.wait_profile_readable(row["name"])
                for row in snapshot["backup"]["profiles"]}

    def profile_checkpoint(self, expected, label):
        snapshot = self.http.snapshot()
        documents = self.full_profiles(snapshot)
        raw, bundle = self.device.backup()
        (self.directory / (label + "-usb.json")).write_bytes(raw)
        atomic_json(self.directory / (label + "-http.json"), snapshot)
        atomic_json(self.directory / (label + "-profiles.json"), documents)
        originals = self.journal.value["profile_documents"]
        wanted = {**originals, **({expected["name"]: expected} if expected is not None else {})}
        if documents != wanted:
            raise BehaviorFailure("Complete saved profiles differ at " + label)
        comparable, comparable_bundle = deepcopy(snapshot), deepcopy(bundle)
        if expected is not None:
            name = expected["name"]
            comparable["backup"]["profiles"] = [row for row in comparable["backup"]["profiles"] if row["name"] != name]
            comparable_bundle["profiles"] = [row for row in comparable_bundle["profiles"] if row["name"] != name]
            if self.journal.owned_profile(documents, snapshot["slots"]) != name:
                raise Incomplete("Temporary profile ownership was not preserved")
        drift, metadata = self.journal.restoration_differences(comparable)
        if drift or not usb.same_bundle(self.journal.value["profiles"], comparable_bundle):
            raise BehaviorFailure("Unrelated configuration changed during profile test: " + ", ".join(drift))
        atomic_json(self.directory / (label + "-comparison.json"), {"differences": drift, "runtime_metadata": metadata})
        return documents

    def profiles(self):
        from behavior_browser import profile_storage_document
        originals = self.journal.value["profile_documents"]
        if len(originals) >= usb.MAX_PROFILE_COUNT:
            raise Incomplete("No temporary profile slot is available; original profiles are preserved")
        name = "bench-" + secrets.token_hex(6)
        if name.lower() in {key.lower() for key in originals}:
            raise Incomplete("Temporary profile identity is already occupied")
        initial = profile_storage_document(name, "Automated acceptance create " + name)
        edited = {**initial, "description": "Automated acceptance edit " + name}
        rejected = profile_storage_document(name, "Rejected duplicate " + name)
        self.device.client.attempts = 1
        token = self.browser.prepare_profile_collision(rejected, list(originals))

        def accepted(label, receipt):
            atomic_json(self.directory / (label + "-browser.json"), receipt)
            if receipt.get("result") == "FAIL":
                raise BehaviorFailure("Shipping profile operation failed: " + label)
            if receipt.get("result") != "PASS":
                raise Incomplete("Shipping profile operation is uncertain: " + label)
            if receipt.get("browser_closed") is not True:
                raise Incomplete("Profile page closure was not confirmed: " + label)
            self.checks.append({"check": label, "result": "PASS"})

        try:
            self.profile_checkpoint(None, "profiles-before-create")
            self.journal.profile_intent("create", None, initial)
            accepted("profiles_create", self.browser.profile_create(initial, list(originals)))
            self.profile_checkpoint(initial, "profiles-created")
            self.journal.profile_intent("edit", initial, edited)
            accepted("profiles_edit", self.browser.profile_edit(initial, edited))
            self.profile_checkpoint(edited, "profiles-edited")
            accepted("profiles_collision", self.browser.profile_collision(token))
            self.profile_checkpoint(edited, "profiles-collision")
        finally:
            self.browser.discard_profile_collision(token)
            self.device.client.attempts = 3
        self.mode("normal")
        self.mode("maintenance")
        self.profile_checkpoint(edited, "profiles-rebooted")
        accepted("profiles_retention", self.browser.profile_reload(edited))
        self.journal.profile_intent("delete", edited, None)
        accepted("profiles_delete", self.browser.profile_delete(edited))
        self.profile_checkpoint(None, "profiles-restored")

    def recovery_write(self, label, endpoint, fields):
        """Retain the one mutation response before independent restoration readback."""
        receipt = {"endpoint": endpoint, "fields": fields, "started_ns": time.monotonic_ns(),
                   "outcome": "unknown"}
        path = self.directory / f"recovery-{label}-{time.time_ns()}.json"
        atomic_json(path, receipt)
        try:
            receipt["response"] = self.http.request(endpoint, fields)
            receipt["outcome"] = "responded"
        except DeviceHttpError as exc:
            receipt.update(outcome="rejected", status=exc.status, response=exc.payload)
        except Exception as exc:
            receipt.update(outcome="unknown", exception_type=type(exc).__name__, error=str(exc))
        receipt["finished_ns"] = time.monotonic_ns()
        atomic_json(path, receipt)

    def restore(self):
        if self.journal is None:
            return "not_needed"
        progress("restoring and checking the original configuration")
        self.mode("maintenance")
        self.http.request("/api/display/preview/clear", {})
        # One authoritative read follows each recovery write; a lost response
        # never causes a second mutation. Preserve third-party edits.
        audio = self.http.request("/api/audio/settings")
        patch = self.journal.audio_restore_patch(audio)
        if patch:
            self.recovery_write("audio", "/api/audio/settings", patch)
            if self.http.request("/api/audio/settings") != {**audio, **patch}:
                raise Incomplete("Original audio fields could not be restored")
        slots = self.http.request("/api/autopush/slots")
        patch = self.journal.autopush_restore(slots)
        if patch:
            self.recovery_write("autopush", "/api/autopush/activate", patch)
            if self.http.request("/api/autopush/slots") != {**slots, "enabled": patch["enable"]}:
                raise Incomplete("Original AutoPush enable state could not be restored")
        if self.journal.value.get("profile_intents"):
            snapshot = self.http.snapshot()
            documents = self.full_profiles(snapshot)
            name = self.journal.owned_profile(documents, snapshot["slots"])
            if name is not None:
                if self.journal.value["profile_intents"][-1]["action"] == "delete":
                    raise Incomplete("A prior profile deletion is unresolved; refusing another delete")
                self.journal.profile_intent("delete", documents[name], None)
                self.recovery_write("profile-delete", "/api/v1/profile/delete", {"name": name})
                try:
                    self.http.wait_profile_readable(name)
                except DeviceHttpError as exc:
                    if exc.status != 404:
                        raise
                else:
                    raise Incomplete("Owned temporary profile deletion could not be verified")
        current = self.http.display()
        patch = self.journal.display_restore_patch(current)
        if patch:
            # Recovery uses a narrow saved-field write, independent of browser health.
            self.recovery_write("display", "/api/display/settings", patch)
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
        drift, metadata = self.journal.restoration_differences(after)
        if not usb.same_bundle(self.journal.value["profiles"], profiles):
            drift.append("usb_profiles")
        if "profile_documents" in self.journal.value:
            drift.extend(differences(self.journal.value["profile_documents"], self.full_profiles(after), "profile_documents"))
        atomic_json(self.directory / f"restoration-{time.time_ns()}.json", {"snapshot": after, "differences": drift,
                                                                      "runtime_metadata": metadata})
        if drift:
            raise Incomplete("Final configuration differs from the saved baseline: " + ", ".join(drift[:12]))
        final = self.mode(self.journal.value["identity"]["mode"])
        self.journal.save(restoration="verified", stage="restored", final_status=final, runtime_metadata=metadata)
        return "verified"

    def execute(self):
        completed, error, restoration = False, None, "not_needed"
        restoration_attempted = False
        browser_shutdown_confirmed = True
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
                    if not browser_shutdown_confirmed:
                        raise Incomplete("Profile browser shutdown is unconfirmed; close its process before recovery")
                    self.device.client.attempts = 3
                    restoration = self.restore()
                except BaseException as exc:
                    restoration = "unverified"
                    cleanup("recovery record", lambda message=str(exc): self.journal.save(
                        restoration=restoration, restoration_error=message))
                    error = error or "Restoration could not be verified: " + str(exc)
                    progress("restoration incomplete; saved recovery record retained")
            elif initial_status is not None:
                # Snapshot admission failed before writes. Still restore the
                # mode changed while attempting that read-only admission.
                cleanup("original mode", lambda: self.mode(initial_status["mode"]))

        try:
            current = self.initial_status()
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
                documents = self.full_profiles(baseline) if self.args.suite in ("profiles", "acceptance") else None
                if getattr(self.args, "campaign_baseline", None):
                    prior = RecoveryJournal.load(self.args.campaign_baseline)
                    drift, _ = prior.restoration_differences(baseline)
                    if not usb.same_bundle(prior.value["profiles"], profiles):
                        drift.append("usb_profiles")
                    if "profile_documents" in prior.value and prior.value["profile_documents"] != documents:
                        drift.append("profile_documents")
                    if drift:
                        raise Incomplete("Configuration changed between cycles: " + ", ".join(drift))
                self.journal = RecoveryJournal.create(self.directory, current, baseline, profiles,
                                                      self.link.identity, connection=self.connection,
                                                      profile_documents=documents)
                if self.args.suite in VISUAL_SUITES or self.args.suite in ("audio", "warning"):
                    self.mode("normal")
                    self.wait(2)
                    self.camera = CaptureSession(self.directory / "camera")
                    self.camera.start()
                    self.mode("maintenance")
                self.browser_open()
                if self.args.suite in ("profiles", "acceptance"):
                    self.profiles()
                if self.args.suite in ("all", "colors", "acceptance"):
                    self.colors()
                if self.args.suite in ("all", "clearance", "acceptance"):
                    self.clearance()
                if self.args.suite in AUDIO_SUITES:
                    from behavior_audio_runner import run_audio_cases
                    self.checks.extend(run_audio_cases(self))
                if self.args.suite == "warning":
                    self.warning()
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
                previous_errors = len(cleanup_errors)
                cleanup("browser shutdown", lambda: self.browser.__exit__(None, None, None))
                if len(cleanup_errors) != previous_errors and self.args.suite in ("profiles", *AUDIO_SUITES):
                    browser_shutdown_confirmed = False
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
                if self.args.suite in VISUAL_SUITES:
                    analyze(self.directory, tolerance=self.args.tolerance, checks=self.checks)
                if self.args.suite in AUDIO_SUITES:
                    from behavior_audio_runner import analyze_audio_cases
                    self.checks.extend(analyze_audio_cases(self.directory))
                if self.args.suite == "warning":
                    self.checks.extend(analyze_warning(self.directory, tolerance=self.args.tolerance))
            except Exception as exc:
                error = error or f"Visual analysis incomplete: {exc}"
        if self.args.command == "run":
            expected = required_checks(self.args.suite)
            names = [row.get("check") for row in self.checks if row.get("check") in expected]
            if set(names) != expected or len(names) != len(expected):
                error = error or "The selected suite's complete, unique checks are required"
        report = {"operation": self.args.command,
                  "result": result_status(self.checks, completed=completed, restoration=restoration, error=error),
                  "candidate": self.expected, "collection_completed": completed,
                  "restoration": restoration, "error": error, "checks": self.checks,
                  "cleanup_errors": cleanup_errors,
                  "firmware_changed": False, "no_device_receipt_latency_claim": True}
        if self.args.command == "run":
            report["scope"] = self.args.suite
            report["required_checks"] = sorted(required_checks(self.args.suite))
            report["full_hardware_coverage"] = False
            report["not_exercised"] = ["physical_touch_and_boot_actuation", "power_supply_faults",
                                       "real_v1_apply", "physical_alp_gps_obd_transports"]
        # A successful recovery does not turn an earlier failed or incomplete
        # qualification into a pass. Keep its summary at the original path.
        result_name = "restoration-result" if self.args.command == "restore" else "result"
        atomic_json(self.directory / f"{result_name}-{time.time_ns()}.json", report)
        atomic_json(self.directory / f"{result_name}.json", report)
        return report


def validate_plan(plan: dict) -> None:
    if plan.get("schema") != 1 or plan.get("suite") not in VISUAL_SUITES:
        raise Incomplete("Unsupported observation plan")
    expected_previews = ({name + suffix for name in ("custom_a", "custom_b", "band_colors")
                          for suffix in ("_saved", "_rebooted")}
                         if plan["suite"] in ("all", "colors", "acceptance") else set())
    previews = plan.get("previews", [])
    if (len(previews) != len(expected_previews)
            or {row.get("label") for row in previews} != expected_previews):
        raise Incomplete("The selected suite's complete preview coverage is required")
    for row in previews:
        if (not 0 <= row["request_after_ns"] - row["request_before_ns"] <= 500_000_000
                or row["ended_ns"] - row["request_after_ns"] < 38_000_000_000):
            raise Incomplete("Preview timing does not cover the declared observations")
    expected_replays = ({f"{scenario}-{persist}" for scenario in ("persistence", "clearance")
                         for persist in (0, 3)} if plan["suite"] in ("all", "clearance", "acceptance") else set())
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


def warning_evidence(directory: Path, plan: dict) -> dict:
    """Bind the quiet optical window to one preserved normal-mode replay."""
    if plan.get("suite") != "warning" or plan.get("capture_completed") is not True:
        raise Incomplete("Warning analysis requires its complete camera plan")
    proof = plan["warning"]
    before = proof["normal_before"]
    if (before != proof["normal_after"] or before["mode"] != "normal" or before["persist"] != 0
            or before["enabled"] is not False or before["git"] != plan["identity"]["git"]
            or before["image"] != plan["identity"]["image"]):
        raise Incomplete("Warning phase boot or firmware identity differs")
    names = {"warning/warning-input.json", "warning/replay_scenario.json",
             "warning/behavior_replay_result.json", "warning/launch-intent.json",
             f"replay-target-{before['boot']}.json"}
    if set(proof["artifacts"]) != names or proof["target_file"] not in names:
        raise Incomplete("Warning artifact inventory differs")
    for name, expected in proof["artifacts"].items():
        path = directory / name
        if path.is_symlink() or path.parent.is_symlink() or hashlib.sha256(path.read_bytes()).hexdigest() != expected:
            raise Incomplete("Warning evidence hash mismatch")
    read = lambda name: json.loads((directory / name).read_text())
    if read("warning/warning-input.json") != warning_scenario():
        raise Incomplete("Warning scenario differs from the fixed regression")
    command = read("warning/launch-intent.json")["command"]
    if ("bench" not in command or "--paused" not in command or "--owner-pid" not in command
            or "--quiet-after-complete" not in command or "--idle-lead" not in command
            or command[command.index("--idle-lead") + 1] != "3" or "--volume" not in command
            or command[command.index("--volume") + 1] != "0,0"):
        raise Incomplete("Warning replay launch lacks its pinned idle/volume contract")
    target = read(proof["target_file"])
    provenance, admitted = target.get("provenance", {}), proof["admitted_target"]
    if (target.get("available") is not True or provenance.get("captureTimedOut") is not False
            or provenance.get("capturedBootId") != before["boot"] or admitted.get("captured_boot") != before["boot"]
            or provenance.get("sessionGeneration") != admitted.get("session_generation")
            or type(admitted.get("session_generation")) is not int or admitted["session_generation"] <= 0
            or target.get("address") != admitted.get("address")
            or not re.fullmatch(r"[0-9A-F]{2}(?::[0-9A-F]{2}){5}", admitted.get("address", ""))):
        raise Incomplete("Warning capture is not bound to the replay boot/session")
    if (proof["audio"].get("voiceVolume") != 0 or proof["audio"].get("speedMuteEnabled") is not False
            or proof["audio"].get("stealthEnabled") is not False or proof["slots"]["enabled"] is not False
            or proof["slots"]["slots"][proof["slots"]["activeSlot"]]["alertPersist"] != 0):
        raise Incomplete("Warning fixture configuration differs")
    replay = read("warning/behavior_replay_result.json")
    delivery = verified_replay_delivery(replay, directory / "warning/replay_scenario.json")
    if delivery["completed_runs"] != 1 or delivery["requested_stimuli"] != 28:
        raise Incomplete("Warning encounter is incomplete")
    resolved = read("warning/replay_scenario.json")
    for index, row in enumerate(resolved["samples"]):
        alerts = row.get("alerts", [])
        if (row.get("offsetSeconds") != index / 3 or row.get("muted") is not False or len(alerts) != 1
                or any(alerts[0].get(key) != value for key, value in
                       {"frequencyMHz": 24150, "band": "k", "bandMask": 4, "strength": 6, "direction": "FRONT",
                        "priority": True, "junk": False}.items())):
            raise Incomplete("Warning resolved stimulus differs")
    events, idle, tail = replay["events"], proof["idle"], proof["tail"]
    volumes = []
    for event in events:
        if event.get("state") != "notification_accepted" or event.get("characteristic") != "B2CE":
            continue
        raw = bytes.fromhex(event.get("payloadHex", ""))
        if len(raw) >= 4 and raw[3] == 0x31:
            if (len(raw) != 15 or raw[:5] != bytes.fromhex("AAD8EA3109") or raw[-1] != 0xAB
                    or raw[-2] != sum(raw[:-2]) % 256 or raw[12] != 0):
                raise Incomplete("Warning display volume differs from zero")
            volumes.append(event["hostMonotonicNs"])
    replies = accepted_capture_replies([row for row in events if row.get("hostMonotonicNs", idle["idle_started_ns"] + 1)
                                      <= idle["idle_started_ns"]], 0)
    if (not volumes or replies is None or replies != idle.get("capture_replies")
            or not any(is_idle_notification(row) and row["hostMonotonicNs"] <= idle["idle_started_ns"] for row in events)
            or idle["release_before_ns"] - idle["idle_started_ns"] < 7_000_000_000
            or not idle["release_before_ns"] <= idle["release_after_ns"] < min(volumes) + 14_000_000_000
            or idle["release_after_ns"] > min(row["requestedHostMonotonicNs"] for row in events
                                              if row.get("state") == "stimulus_requested")):
        raise Incomplete("Warning zero-volume and idle readiness evidence is incomplete")
    bundle = terminal_clear_bundle(events)
    if (bundle is None or bundle[0]["hostMonotonicNs"] != tail["clear_accepted_ns"]
            or bundle[1]["hostMonotonicNs"] != tail["idle_accepted_ns"]
            or tail["observed_until_ns"] - tail["idle_accepted_ns"] < 30_000_000_000
            or tail.get("later_accepted_notifications") != 0
            or any(row.get("state") == "notification_accepted" or
                   (row.get("state") == "session_transport" and row.get("active") is False
                    and row.get("observerHostMonotonicNs", 0) < tail["observed_until_ns"])
                   for row in events[events.index(bundle[1]) + 1:])):
        raise Incomplete("Warning observation was not connected packet silence")
    return {**proof, "first_zero_ns": min(volumes),
            "expected_onset_ns": max(min(volumes) + 15_000_000_000, tail["clear_accepted_ns"])}


def _warning_native_frames(frames: RecordedFrames, target_ns: int) -> list[dict]:
    """Decode every retained source frame in the bounded transition neighborhood."""
    first = bisect_left(frames.host, target_ns - 25_000_000)
    stop = bisect_right(frames.host, target_ns + 25_000_000)
    indices = list(range(first, stop))
    if not indices or target_ns not in frames.host[first:stop]:
        raise CaptureError("Original unreadable warning frame is missing")
    for previous, index in zip(indices, indices[1:]):
        if (frames.rows[index]["frame_seq"] != frames.rows[previous]["frame_seq"] + 1
                or frames.host[index] <= frames.host[previous]):
            raise CaptureError("Native warning frame coverage is missing or unordered")
    result = []
    for index in indices:
        actual_pts, rgb = frames.decoder(frames.video, frames.pts[index])
        if not abs(actual_pts - frames.pts[index]) <= .00001:
            raise CaptureError("Warning transition decoder did not return the exact source frame")
        result.append({"time_s": frames.host[index] / 1e9,
                       "host_capture_ns": frames.host[index], "video_pts_seconds": actual_pts,
                       "frame_seq": frames.rows[index]["frame_seq"],
                       "source_frame": dict(frames.rows[index]), "rgb": rgb})
    return result


def analyze_warning(directory: Path, *, tolerance: float = .35) -> list[dict]:
    from behavior_visual import observe_warning_frame, evaluate_warning, evaluate_warning_transition
    plan = json.loads((directory / "observations.json").read_text())
    if tolerance != plan.get("timing_tolerance_s"):
        raise Incomplete("Warning analysis cannot change its declared tolerance")
    proof = warning_evidence(directory, plan)
    frames = RecordedFrames(directory / "camera")
    def observations(start, end):
        return [{"time_s": row["host_monotonic_ns"] / 1e9,
                 "video_pts_seconds": row["video_pts_seconds"],
                 **observe_warning_frame(row["rgb"], frames.registration)}
                for row in frames.decoded_samples(round(start), round(end), step_s=.1)]
    idle = proof["idle"]
    reference = observations(idle["release_before_ns"] - 1_500_000_000, idle["release_before_ns"] - 300_000_000)
    onset = proof["expected_onset_ns"]
    clear = proof["tail"]["idle_accepted_ns"]
    live = observations(clear - 1_100_000_000, clear - 200_000_000)
    samples = observations(max(clear + 50_000_000, onset - 1_000_000_000),
                           onset + 12_500_000_000)
    checks = evaluate_warning(samples, reference, onset_s=onset / 1e9, tolerance_s=tolerance,
                              clear_idle_s=clear / 1e9, live_control=live)
    transitions = {}
    for row in samples:
        if (row.get("warning_state") != "unreadable"
                or not onset / 1e9 + tolerance <= row["time_s"] <= onset / 1e9 + 10. - tolerance):
            continue
        try:
            native = _warning_native_frames(frames, round(row["time_s"] * 1e9))
            transition = evaluate_warning_transition(native, frames.registration, reference, row["time_s"])
            transition["native_frames"] = [{key: value for key, value in item.items() if key != "rgb"}
                                            for item in native]
        except CaptureError as exc:
            transition = {"result": "INCOMPLETE", "target_s": row["time_s"], "reason": str(exc)}
        transitions[row["time_s"]] = transition
    presence_error = None
    try:
        presence = frames.warning_presence(round(onset + tolerance * 1e9),
                                           round(onset + (10. - tolerance) * 1e9))
    except CaptureError as exc:
        presence, presence_error = [], str(exc)
    checks = evaluate_warning(samples, reference, onset_s=onset / 1e9, tolerance_s=tolerance,
                              clear_idle_s=clear / 1e9, live_control=live, transitions=transitions,
                              native_presence=presence)
    atomic_json(directory / "warning-pixels.json", {"reference": reference, "live": live, "samples": samples})
    atomic_json(directory / "warning-presence.json", {"frames": presence, "error": presence_error})
    atomic_json(directory / "warning-transitions.json", {"transitions": list(transitions.values())})
    atomic_json(directory / "warning-result.json", {"checks": checks, "analysis_completed": True})
    return checks


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


def run_cycles(args, directory: Path, runner_factory=Runner) -> dict:
    """Fixed complete suite cycles; never resume an interrupted mutation phase."""
    manifest = {"schema": 1, "suite": args.suite, "requested_cycles": args.cycles,
                "required_checks": sorted(required_checks(args.suite)), "cycles": [],
                "active_cycle": None, "result": "INCOMPLETE"}
    atomic_json(directory / "campaign.json", manifest)
    error = None
    try:
        for number in range(1, args.cycles + 1):
            name = f"cycle-{number:04d}"
            child = private_output(str(directory / name))
            manifest["active_cycle"] = name
            atomic_json(directory / "campaign.json", manifest)
            options = argparse.Namespace(**vars(args))
            options.campaign_baseline = str(directory / "cycle-0001") if number > 1 else None
            result = runner_factory(options, child).execute()
            if result["result"] == "PASS":
                expected = required_checks(args.suite)
                names = [row.get("check") for row in result.get("checks", [])]
                if (result.get("scope") != args.suite or set(names) != expected
                        or len(names) != len(expected)
                        or any(row.get("result") != "PASS" for row in result["checks"])):
                    raise Incomplete("Cycle claimed success without its complete unique passing checks")
            manifest["cycles"].append({"name": name, "result": result["result"],
                                       "restoration": result["restoration"]})
            atomic_json(directory / "campaign.json", manifest)
            if result["result"] != "PASS" or result["restoration"] != "verified":
                break
            manifest["active_cycle"] = None
            atomic_json(directory / "campaign.json", manifest)
    except BaseException as exc:
        error = f"{type(exc).__name__}: {exc}"
    rows = manifest["cycles"]
    manifest["result"] = ("FAIL" if any(row["result"] == "FAIL" for row in rows) else
                          "PASS" if not error and len(rows) == args.cycles and
                          all(row["result"] == "PASS" and row["restoration"] == "verified" for row in rows)
                          else "INCOMPLETE")
    manifest["error"] = error
    manifest["full_hardware_coverage"] = False
    atomic_json(directory / "campaign.json", manifest)
    report = {**manifest, "operation": "run", "restoration":
              "verified" if rows and all(row["restoration"] == "verified" for row in rows) and not error else "unverified"}
    atomic_json(directory / "result.json", report)
    return report


def restore_campaign(args, directory: Path, runner_factory=Runner) -> dict:
    campaign = json.loads((directory / "campaign.json").read_text())
    active = campaign.get("active_cycle")
    if (campaign.get("schema") != 1 or type(campaign.get("requested_cycles")) is not int
            or not 1 <= campaign["requested_cycles"] <= 100):
        raise Incomplete("Invalid campaign recovery record")
    if active is None:
        raise Incomplete("Campaign has no interrupted cycle to restore")
    if not isinstance(active, str) or not re.fullmatch(r"cycle-[0-9]{4}", active):
        raise Incomplete("Invalid interrupted cycle identity")
    if not 1 <= int(active[6:]) <= campaign["requested_cycles"]:
        raise Incomplete("Interrupted cycle exceeds the declared campaign")
    child = private_output(str(directory / active), existing=True)
    result = runner_factory(args, child).execute()
    atomic_json(directory / "restoration-result.json", {**result, "cycle": active})
    # campaign.json/result.json remain the original qualification outcome.
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("run", "restore", "analyze"))
    parser.add_argument("--out-dir", required=True)
    parser.add_argument("--port")
    parser.add_argument("--git", default="")
    parser.add_argument("--image", default="")
    parser.add_argument("--firmware", help="retained qualified firmware.bin with sibling identity.json")
    parser.add_argument("--candidate-manifest", help="retained artifact identity/CI manifest")
    parser.add_argument("--base-url", help="maintenance origin; restore defaults to the saved run connection")
    parser.add_argument("--ssid", help="maintenance Wi-Fi; restore defaults to the saved run connection")
    parser.add_argument("--suite", choices=("all", "colors", "clearance", "profiles", "audio", "acceptance", "warning"), default="all")
    parser.add_argument("--cycles", type=int, default=1, help="complete independent suite cycles, each restored before advancing")
    parser.add_argument("--microphone", help="explicit external acoustic capture input for audio/acceptance")
    parser.add_argument("--real-v1-off", action="store_true", help="attest the real detector is off for controlled emulator stimuli")
    parser.add_argument("--headed", action="store_true")
    parser.add_argument("--tolerance", type=float, default=.35,
                        help="predeclared optical observation allowance, not a device latency guarantee")
    args = parser.parse_args(argv)
    os.umask(0o077)
    if not .1 <= args.tolerance <= .5:
        parser.error("observation tolerance must be 0.1 through 0.5 seconds")
    if not 1 <= args.cycles <= 100:
        parser.error("cycles must be 1 through 100")
    if args.command != "run" and args.cycles != 1:
        parser.error("cycles is a run-only selection")
    if args.command == "run" and args.suite in AUDIO_SUITES:
        if not (args.candidate_manifest or args.firmware):
            parser.error(args.suite + " requires a retained qualified candidate manifest or firmware")
        if not args.microphone or not args.real_v1_off:
            parser.error(args.suite + " requires a named microphone and real-V1-off fixture attestation")
    if args.command == "run" and args.suite == "warning":
        if not (args.candidate_manifest or args.firmware) or not args.real_v1_off:
            parser.error("warning requires a retained qualified candidate and real-V1-off attestation")
    if args.command == "run" and (not re.fullmatch(r"[0-9a-f]{7,40}", args.git)
                                   or not re.fullmatch(r"[0-9a-f]{9}", args.image)):
        parser.error("run requires a pinned --git and --image identity")
    directory = private_output(args.out_dir, existing=args.command != "run")
    with (directory / ".runner.lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        if args.command == "analyze":
            if (directory / "campaign.json").exists():
                raise Incomplete("Campaign reanalysis is not supported; original child qualification results remain authoritative")
            plan = json.loads((directory / "observations.json").read_text())
            if plan.get("suite") in ("profiles", *AUDIO_SUITES):
                raise Incomplete("This command analyzes display-only suites; it cannot requalify an acceptance/profile/audio run")
            checks = (analyze_warning(directory, tolerance=args.tolerance) if plan.get("suite") == "warning"
                      else analyze(directory, tolerance=args.tolerance))
            return 1 if any(c["result"] == "FAIL" for c in checks) else 0 if checks and all(c["result"] == "PASS" for c in checks) else 2
        if args.command == "run":
            # At the fixed 80 Mbps camera profile, the measured clearance leg
            # uses about 3.5 GB including admission. Keep the 1 GB live reserve.
            minimum_gb = {"all": 12, "colors": 7, "clearance": 5, "profiles": 1, "audio": 12, "acceptance": 24, "warning": 5}[args.suite]
            require_space(directory, minimum_gb * args.cycles * 1_000_000_000)
            if args.suite in ("all", "clearance", "warning", *AUDIO_SUITES) and not (ROOT / "tools/v1replay/.build/v1replay").is_file():
                raise Incomplete("Build the existing v1replay host tool before running")
            if args.candidate_manifest or args.firmware:
                qualify_candidate(args, directory)
            if args.suite in AUDIO_SUITES:
                from behavior_audio import prepare_microphone_recorder
                prepare_microphone_recorder()
        def stop(_signal, _frame):
            raise KeyboardInterrupt("Interrupted; restoring owned settings")
        signal.signal(signal.SIGTERM, stop)
        with DutLease():
            if args.command == "restore" and (directory / "campaign.json").exists():
                result = restore_campaign(args, directory)
            elif args.command == "run" and args.cycles > 1:
                result = run_cycles(args, directory)
            else:
                result = Runner(args, directory).execute()
        progress(f"{args.command}: {result['result']}; restoration={result['restoration']}; evidence={directory}")
        return {"PASS": 0, "FAIL": 1, "INCOMPLETE": 2}[result["result"]]


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (Incomplete, CaptureError, OSError, ValueError) as exc:
        progress("INCOMPLETE: " + str(exc))
        raise SystemExit(2)
