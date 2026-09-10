#!/usr/bin/env python3
"""Focused regressions for the external-only bench evidence contract."""

from __future__ import annotations

import contextlib
import fcntl
import hashlib
import io
import itertools
import json
import os
import select
import stat
import struct
import subprocess
import sys
import tempfile
import textwrap
from pathlib import Path
from types import SimpleNamespace
from typing import Any
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts" / "bench"))

import run_window as run_window_module  # noqa: E402
from run_window import (  # noqa: E402
    BENCH_TIMELINE_NAME,
    REPLAY_DELIVERY_NAME,
    REPLAY_STIMULUS_NAME,
    BenchTimeline,
    RuntimeIdentityFailure,
    RuntimeIdentityTracker,
    V1Emulator,
    V1RadioLease,
    establish_serial_boundary,
    collect_post_window_configuration,
    file_artifact,
    parse_runtime_boot_identity,
    notification_delivery_problem,
    post_window_configuration_timeout_s,
    publish_replay_delivery_evidence,
    publish_replay_stimulus_evidence,
    qualify_runtime_identity,
    resolve_runner_log_paths,
    summarize_notification_delivery,
)


def assert_true(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def test_file_artifact_owns_raw_bytes() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "raw.ndjson"
        payload = b'{"sample":1}\n'
        path.write_bytes(payload)
        artifact = file_artifact(path)
        assert_true(artifact["path"] == path.name, str(artifact))
        assert_true(artifact["size_bytes"] == len(payload), str(artifact))
        assert_true(
            artifact["sha256"] == hashlib.sha256(payload).hexdigest(), str(artifact)
        )


def test_build_artifacts_retain_exact_application_after_build_cache_changes() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        recording, image, window, _ = resident_fixture(root)
        build = root / "build-cache"
        build.mkdir()
        for name in run_window_module.BUILD_UPLOAD_FILES:
            (build / name).write_bytes(("synthetic build output: " + name).encode())
        app = bytearray(image.read_bytes())
        app[176:208] = bytes.fromhex(run_window_module.sha256_file(build / "firmware.elf"))
        original = build / "firmware.bin"
        original.write_bytes(app)
        result = run_window_module.retain_build_upload_artifacts(recording, build, upload_performed=True)
        manifest = json.loads((recording / run_window_module.BUILD_UPLOAD_ARTIFACTS_NAME).read_text())
        record = next(item for item in manifest["files"] if item["name"] == "firmware.bin")
        retained = recording / record["path"]
        assert_true(record["path"] == "firmware.bin" and record["size_bytes"] == len(app)
                    and record["sha256"] == hashlib.sha256(app).hexdigest(), str(record))
        assert_true(file_artifact(retained) == {key: record[key] for key in ("path", "size_bytes", "sha256")}, str(record))
        assert_true(not original.samefile(retained), "retained application aliases mutable build output")
        assert_true(result["schema_version"] == 1 and result["missing"] == [], str(result))
        assert_true(all("path" not in item for item in manifest["files"] if item["name"] != "firmware.bin"),
                    "retention expanded to unrelated build outputs")
        original.write_bytes(b"next build replaced this image")
        assert_true(retained.read_bytes() == app, "later build changed retained application")
        original.unlink()
        image.unlink()
        assert_true(retained.read_bytes() == app, "cache cleanup removed retained application")
        # Exercise the actual no-flash reference consumer with the newly retained
        # binary. The miniature image is a host fixture, not target firmware proof.
        identity = {**window["runtime_identity"], "image_id": result["expected_runtime_image_id"]}
        serial_path = recording / "bench_serial.log"
        serial_path.write_text(f"BOOT bootId=42 git={identity['git_sha']} image={identity['image_id']}\n")
        window["runtime_identity"] = identity
        window["artifacts"]["bench_serial"] = file_artifact(serial_path)
        window["runtime_qualification"] = qualify_runtime_identity(
            identity, intended_git_sha=window["git_sha"], build_upload=manifest, upload=True)
        assert_true(window["runtime_qualification"]["status"] == "qualified", str(window))
        write_resident_fixture(recording, window, manifest)
        fresh = root / "fresh"
        fresh.mkdir()
        reference = run_window_module.retain_resident_artifacts(recording, retained, fresh)
        copied = fresh / reference["resident_provenance"]["reference_files"]["firmware.bin"]["path"]
        assert_true(copied.read_bytes() == app, "retained application is unusable for the exact-image reference")


def test_build_application_retention_refuses_missing_empty_changed_or_failed_copy() -> None:
    for failure in ("missing", "empty", "hash", "size", "copy", "existing"):
        with tempfile.TemporaryDirectory() as tmp, contextlib.ExitStack() as patches:
            root = Path(tmp)
            build, recording = root / "build-cache", root / "recording"
            build.mkdir(); recording.mkdir()
            original, retained = build / "firmware.bin", recording / "firmware.bin"
            if failure != "missing":
                original.write_bytes(b"" if failure == "empty" else b"original application")
            if failure in ("hash", "size"):
                real_sha = run_window_module.sha256_file
                def change_source(path: Path) -> str:
                    digest = real_sha(path)
                    if path == original:
                        path.write_bytes(b"X" * path.stat().st_size if failure == "hash" else b"short")
                    return digest
                patches.enter_context(mock.patch.object(run_window_module, "sha256_file", side_effect=change_source))
            elif failure == "copy":
                real_open = Path.open
                def fail_copy(path: Path, *args: Any, **kwargs: Any) -> Any:
                    if path == retained and args == ("xb",):
                        raise OSError("copy destination unavailable")
                    return real_open(path, *args, **kwargs)
                patches.enter_context(mock.patch.object(Path, "open", fail_copy))
            elif failure == "existing":
                retained.write_bytes(b"previously retained application")
            try:
                run_window_module.retain_build_upload_artifacts(recording, build, upload_performed=True)
            except (OSError, RuntimeError):
                pass
            else:
                raise AssertionError("retention accepted " + failure)
            assert_true(not (recording / run_window_module.BUILD_UPLOAD_ARTIFACTS_NAME).exists(),
                        "failed retention published a trusted manifest: " + failure)
            if failure == "existing":
                assert_true(retained.read_bytes() == b"previously retained application", "retention overwrote prior bytes")


def test_live_collection_refuses_a_retained_application_before_starting() -> None:
    for dangling_link in (False, True):
        with tempfile.TemporaryDirectory() as tmp:
            out_dir = Path(tmp)
            retained = out_dir / "firmware.bin"
            if dangling_link:
                retained.symlink_to(out_dir / "unavailable.bin")
            else:
                retained.write_bytes(b"partial or complete previous application")
            result = subprocess.run(
                [sys.executable, str(ROOT / "scripts/bench/run_window.py"),
                 "--suite", "replay", "--out-dir", str(out_dir), "--upload",
                 "--git-worktree-clean", "1", "--replay-executable", "/unused/replay"],
                capture_output=True, text=True,
            )
            assert_true(result.returncode == 3, result.stderr)
            assert_true("refusing to reuse existing live evidence: firmware.bin" in result.stderr, result.stderr)
            assert_true(list(out_dir.iterdir()) == [retained], "refusal added artifacts to the original recording")
            if dangling_link:
                assert_true(retained.is_symlink() and retained.readlink() == out_dir / "unavailable.bin",
                            "refusal changed the occupied path")
            else:
                assert_true(retained.read_bytes() == b"partial or complete previous application",
                            "refusal changed the original recording")


def test_reused_live_output_refusal_preserves_existing_evidence() -> None:
    for extra_args in ([], ["--duration-seconds", "0"]):
        with tempfile.TemporaryDirectory() as tmp:
            out_dir = Path(tmp)
            retained = {
                "window_result.json": b'{"result":"PASS","sentinel":"original run"}\n',
                "bench_serial.log": b"original serial bytes\n",
            }
            for name, data in retained.items():
                (out_dir / name).write_bytes(data)
            result = subprocess.run(
                [sys.executable, str(ROOT / "scripts/bench/run_window.py"),
                 "--suite", "replay", "--out-dir", str(out_dir),
                 "--git-worktree-clean", "1", "--replay-executable", "/unused/replay",
                 *extra_args],
                capture_output=True, text=True,
            )
            assert_true(result.returncode == 3, result.stderr)
            assert_true("refusing to reuse existing live evidence" in result.stderr, result.stderr)
            assert_true(set(path.name for path in out_dir.iterdir()) == set(retained),
                        "refusal created new artifacts in an existing recording")
            for name, data in retained.items():
                assert_true((out_dir / name).read_bytes() == data,
                            f"refusal overwrote retained {name}")


def test_replay_stimulus_is_persisted_as_raw_ndjson_once() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        out_dir = Path(tmp)
        events = [
            {"state": "stimulus_requested", "sample": 1},
            {"state": "stimulus_requested", "sample": 2},
        ]
        emulator = {"stimulus_events": list(events)}
        result = publish_replay_stimulus_evidence(
            emulator, out_dir, suite="replay"
        )
        assert_true(result is not None and result["status"] == "captured", str(result))
        assert_true("stimulus_events" not in emulator, "events remained duplicated")
        path = out_dir / REPLAY_STIMULUS_NAME
        decoded = [json.loads(line) for line in path.read_text().splitlines()]
        assert_true(decoded == events, f"raw stimulus events changed: {decoded}")
        try:
            publish_replay_stimulus_evidence(
                {"stimulus_events": list(events)}, out_dir, suite="replay"
            )
        except FileExistsError:
            pass
        else:
            raise AssertionError("existing raw stimulus evidence was overwritten")


def test_replay_delivery_is_persisted_with_explicit_loss_denominators() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        out_dir = Path(tmp)
        events = [
            {"state": "notification_requested", "globalTxSequence": 1},
            {"state": "notification_dropped", "globalTxSequence": 1},
        ]
        emulator = {"delivery_events": list(events)}
        result = publish_replay_delivery_evidence(
            emulator, out_dir, suite="replay"
        )
        assert_true(result is not None and result["event_count"] == 2, str(result))
        assert_true("delivery_events" not in emulator, "delivery events remained duplicated")
        decoded = [
            json.loads(line)
            for line in (out_dir / REPLAY_DELIVERY_NAME).read_text().splitlines()
        ]
        assert_true(decoded == events, f"raw delivery events changed: {decoded}")


def test_delivery_summary_counts_attempts_and_vetoes_dropped_or_skipped() -> None:
    clean = summarize_notification_delivery(
        [
            {"state": "notification_requested", "globalTxSequence": 7},
            {"state": "notification_delayed", "globalTxSequence": 7},
            {"state": "notification_accepted", "globalTxSequence": 7},
        ]
    )
    assert_true(clean["requested"] == 1, str(clean))
    assert_true(clean["attempted"] == 2, str(clean))
    assert_true(clean["delivered"] == 1, str(clean))
    assert_true(notification_delivery_problem(clean) == "", str(clean))

    for loss_state in ("notification_dropped", "notification_skipped"):
        lossy = summarize_notification_delivery(
            [
                {"state": "notification_requested", "globalTxSequence": 8},
                {"state": loss_state, "globalTxSequence": 8},
            ]
        )
        problem = notification_delivery_problem(lossy)
        assert_true(lossy["complete"] is False, str(lossy))
        assert_true(loss_state.removeprefix("notification_") in problem, problem)


def test_delivery_summary_vetoes_malformed_sequence_lifecycles() -> None:
    malformed_streams = {
        "duplicate requested": [
            {"state": "notification_requested", "globalTxSequence": 1},
            {"state": "notification_requested", "globalTxSequence": 1},
            {"state": "notification_accepted", "globalTxSequence": 1},
        ],
        "orphan terminal": [
            {"state": "notification_accepted", "globalTxSequence": 2},
        ],
        "terminal before request": [
            {"state": "notification_accepted", "globalTxSequence": 3},
            {"state": "notification_requested", "globalTxSequence": 3},
        ],
        "duplicate terminal": [
            {"state": "notification_requested", "globalTxSequence": 4},
            {"state": "notification_accepted", "globalTxSequence": 4},
            {"state": "notification_accepted", "globalTxSequence": 4},
        ],
        "delayed after terminal": [
            {"state": "notification_requested", "globalTxSequence": 5},
            {"state": "notification_accepted", "globalTxSequence": 5},
            {"state": "notification_delayed", "globalTxSequence": 5},
        ],
        "missing identity": [
            {"state": "notification_requested"},
            {"state": "notification_accepted"},
        ],
        "zero identity": [
            {"state": "notification_requested", "globalTxSequence": 0},
            {"state": "notification_accepted", "globalTxSequence": 0},
        ],
    }
    for name, events in malformed_streams.items():
        summary = summarize_notification_delivery(events)
        problem = notification_delivery_problem(summary)
        assert_true(summary["complete"] is False, f"{name}: {summary}")
        assert_true("malformed" in problem, f"{name}: {problem}")


def test_delivery_summary_vetoes_empty_instrumentation_stream() -> None:
    summary = summarize_notification_delivery([])
    problem = notification_delivery_problem(summary, required=True)
    assert_true(summary["complete"] is False, str(summary))
    assert_true(summary["requested"] == 0, str(summary))
    assert_true("evidence is empty" in problem, problem)
    assert_true(
        notification_delivery_problem(summary, required=False) == "",
        "idle emulator suites unexpectedly require replay delivery evidence",
    )


def test_runner_logs_are_confined_to_the_run_directory() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        out_dir = Path(tmp)
        args = SimpleNamespace(
            runner_stdout_log=str(out_dir / "run.log"),
            runner_stderr_log=str(out_dir / "run.err"),
        )
        paths = resolve_runner_log_paths(args, out_dir)
        assert_true(paths["stdout"] == (out_dir / "run.log").resolve(), str(paths))
        args.runner_stdout_log = str(out_dir / "elsewhere.log")
        try:
            resolve_runner_log_paths(args, out_dir)
        except ValueError:
            pass
        else:
            raise AssertionError("runner stdout escaped its exact owned path")


def test_timeline_keeps_ordered_external_events_and_scrubs_private_paths() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / BENCH_TIMELINE_NAME
        timeline = BenchTimeline(path)
        timeline.record("serial_line", value="/Users/private/operator")
        timeline.record_external({"state": "stimulus_requested", "sample": 7}, "v1replay")
        timeline.close()
        text = path.read_text(encoding="utf-8")
        assert_true("/Users/private/operator" not in text, "timeline leaked a private path")
        records = [json.loads(line) for line in text.splitlines()]
        assert_true(records[0]["event"] == "timeline_opened", str(records))
        assert_true(records[-1]["event"] == "timeline_closed", str(records))
        assert_true(
            any(
                record.get("payload", {}).get("state") == "stimulus_requested"
                for record in records
            ),
            "external machine event was not retained",
        )


class TailClock:
    def __init__(self) -> None:
        self.now = 0.0

    def __call__(self) -> float:
        return self.now


class TailObserver:
    def __init__(
        self,
        timeline: BenchTimeline,
        lines: list[str],
        *,
        reboot_on_read: bool = False,
    ) -> None:
        self.timeline = timeline
        self.lines = list(lines)
        self.reboot_on_read = reboot_on_read
        self.reset_requested_ns = 1_000_000_000
        self.runtime_identity = {"boot_id": 7}
        self.boot_marker_count = 1
        self.line_count = 0
        self.last_receive_ns: int | None = None
        self.clock: TailClock | None = None
        self.receive_ns: list[int] = []

    def read_line(self, _timeout_s: float) -> str:
        assert self.clock is not None
        self.clock.now += 0.1
        if not self.lines:
            return ""
        line = self.lines.pop(0)
        self.line_count += 1
        received = self.timeline.record("serial_receive", line=line)
        self.last_receive_ns = (
            self.receive_ns.pop(0)
            if self.receive_ns
            else received["host_monotonic_ns"]
        )
        if self.reboot_on_read:
            self.boot_marker_count += 1
        return line


def cfg_line(*, boot_id: int = 7, uptime_ms: int = 1000, revision: int = 2) -> str:
    return (
        f"CFG bootId={boot_id} uptimeMs={uptime_ms} revision={revision} "
        "activeSlot=1 stealthEnabled=0 priorityArrowOnly=1 "
        "alertPersistenceSeconds=0"
    )


def test_post_window_configuration_requires_conservative_same_boot_emission() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        timeline = BenchTimeline(Path(tmp) / BENCH_TIMELINE_NAME)
        clock = TailClock()
        observer = TailObserver(
            timeline,
            [cfg_line(uptime_ms=1000), cfg_line(uptime_ms=3000)],
        )
        observer.clock = clock
        result = collect_post_window_configuration(
            observer,  # type: ignore[arg-type]
            required_after_ns=3_000_000_000,
            timeout_s=1.0,
            timeline=timeline,
            monotonic=clock,
        )
        timeline.close()
        assert_true(result["status"] == "verified", str(result))
        assert_true(result["snapshot_uptime_ms"] == 3000, str(result))
        records = [json.loads(line) for line in timeline.path.read_text().splitlines()]
        assert_true(
            any(
                record.get("event") == "serial_receive"
                and "uptimeMs=3000" in record.get("line", "")
                for record in records
            ),
            "qualifying raw CFG line was not retained in the owned timeline",
        )


def test_post_window_configuration_fails_closed() -> None:
    cases = {
        "old uptime": ([cfg_line(uptime_ms=1000)], False, []),
        "wrong boot": ([cfg_line(boot_id=8, uptime_ms=3000)], False, []),
        "malformed": (["CFG bootId=7"], False, []),
        "reboot": ([cfg_line(uptime_ms=3000)], True, []),
        "future uptime": ([cfg_line(uptime_ms=3000)], False, [2_000_000_000]),
    }
    for name, (lines, reboot, receive_ns) in cases.items():
        with tempfile.TemporaryDirectory() as tmp:
            timeline = BenchTimeline(Path(tmp) / BENCH_TIMELINE_NAME)
            clock = TailClock()
            observer = TailObserver(timeline, lines, reboot_on_read=reboot)
            observer.clock = clock
            observer.receive_ns = list(receive_ns)
            try:
                collect_post_window_configuration(
                    observer,  # type: ignore[arg-type]
                    required_after_ns=3_000_000_000,
                    timeout_s=0.5,
                    timeline=timeline,
                    monotonic=clock,
                )
            except RuntimeError:
                pass
            else:
                raise AssertionError(f"{name} post-window evidence was accepted")
            finally:
                timeline.close()


def test_post_window_configuration_timeout_covers_clock_allowance() -> None:
    assert_true(post_window_configuration_timeout_s(1) == 10.0, "short tail")
    assert_true(post_window_configuration_timeout_s(300) == 10.0, "five-minute tail")
    assert_true(post_window_configuration_timeout_s(3600) > 75.0, "long-run clock tail")


def _write_executable(path: Path) -> None:
    path.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
    path.chmod(path.stat().st_mode | stat.S_IXUSR)


def capture_replay_command(scenario: str, reader_qualification: bool) -> list[str]:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        executable = root / "build" / "v1replay"
        executable.parent.mkdir()
        _write_executable(executable)
        out_dir = root / "run"
        out_dir.mkdir()
        captured: list[str] = []

        class FakeProcess:
            pid = 1234

            def poll(self) -> None:
                return None

        original = run_window_module.subprocess.Popen

        def fake_popen(command: list[str], **_kwargs: Any) -> FakeProcess:
            captured.extend(command)
            return FakeProcess()

        run_window_module.subprocess.Popen = fake_popen  # type: ignore[assignment]
        emulator = V1Emulator(
            executable,
            out_dir,
            "replay",
            "scenario",
            lease_fd=9,
            scenario=scenario,
            machine_event=lambda _payload: None,
            reader_qualification=reader_qualification,
        )
        try:
            emulator.start()
        finally:
            if emulator.log_handle is not None:
                emulator.log_handle.close()
            run_window_module.subprocess.Popen = original

        assert_true("--machine-events" in captured, str(captured))
        assert_true("--scenario-evidence" in captured, str(captured))
        assert_true("--owner-pid" in captured, str(captured))
        return captured


def test_replay_process_requests_raw_machine_and_scenario_evidence() -> None:
    for scenario, qualification in (("fixture.json", False), ("", False), ("", True)):
        command = capture_replay_command(scenario, qualification)
        assert_true(("--reader-qualification" in command) is qualification, str(command))
        assert_true(("--scenario" in command) is bool(scenario), str(command))
    for suite, scenario in (("core", ""), ("display", ""), ("replay", "fixture.json")):
        try:
            V1Emulator(Path("fixture"), Path("fixture"), suite, "scenario", lease_fd=9,
                       scenario=scenario, reader_qualification=True,
                       machine_event=lambda _payload: None)
        except ValueError as exc:
            assert_true("reader qualification requires replay" in str(exc), str(exc))
        else:
            raise AssertionError("reader qualification accepted incompatible stimulus")


def finish_replay_fixture(states: list[str]) -> dict[str, Any]:
    with tempfile.TemporaryDirectory() as tmp:
        out_dir = Path(tmp)
        executable = out_dir / "v1replay"
        _write_executable(executable)

        class FakeProcess:
            pid = 1234

            def __init__(self) -> None:
                self.running = True

            def poll(self) -> int | None:
                return None if self.running else 0

            def send_signal(self, _signal: int) -> None:
                pass

            def wait(self, timeout: int) -> int:
                del timeout
                self.running = False
                return 0

        emulator = V1Emulator(
            executable,
            out_dir,
            "replay",
            "scenario",
            lease_fd=9,
            scenario="fixture.json",
            machine_event=lambda _payload: None,
        )
        emulator.process = FakeProcess()  # type: ignore[assignment]
        lines = []
        for state in states:
            event: dict[str, Any] = {"state": state, "fixture": True}
            if state.startswith("notification_"):
                event["globalTxSequence"] = 1
            lines.append("V1REPLAY_EVENT " + json.dumps(event))
        emulator.log_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
        return emulator.finish(window_completed=True)


def test_requested_dropped_complete_stopped_is_not_completed() -> None:
    result = finish_replay_fixture(
        ["notification_requested", "notification_dropped", "complete", "stopped"]
    )
    delivery = result["notification_delivery"]
    assert_true(result["lifecycle_completed"] is True, str(result))
    assert_true(result["completed"] is False, str(result))
    assert_true(delivery["requested"] == 1, str(delivery))
    assert_true(delivery["delivered"] == 0, str(delivery))
    assert_true(delivery["dropped"] == 1, str(delivery))


def test_emulator_cleanup_before_start_preserves_primary_failure() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        emulator = V1Emulator(Path("unused"), Path(tmp), "replay", "scenario", lease_fd=9, scenario="",
                              machine_event=lambda _payload: None)
        result = emulator.finish(window_completed=False)
        assert_true(result["started"] is False and result["completed"] is False, str(result))
        assert_true(result["lifecycle_completed"] is False, str(result))
        assert_true(not emulator.log_path.exists(), "cleanup invented replay evidence")
        emulator.process = SimpleNamespace(poll=lambda: 0)
        try:
            emulator.finish(window_completed=False)
        except FileNotFoundError:
            pass
        else:
            raise AssertionError("missing started-emulator evidence was suppressed")


def test_requested_accepted_complete_stopped_is_completed() -> None:
    result = finish_replay_fixture(
        ["notification_requested", "notification_accepted", "complete", "stopped"]
    )
    delivery = result["notification_delivery"]
    assert_true(result["lifecycle_completed"] is True, str(result))
    assert_true(result["completed"] is True, str(result))
    assert_true(delivery["requested"] == 1, str(delivery))
    assert_true(delivery["attempted"] == 1, str(delivery))
    assert_true(delivery["delivered"] == 1, str(delivery))


def test_radio_lease_excludes_concurrent_owners_and_rejects_symlink_parent() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        lock_path = root / "state" / "radio.lock"
        lock_path.parent.mkdir()
        with V1RadioLease(lock_path, quiet_seconds=0) as owner:
            assert_true(owner.fd is not None, "radio lease has no descriptor")
            try:
                with V1RadioLease(lock_path, quiet_seconds=0):
                    pass
            except RuntimeError:
                pass
            else:
                raise AssertionError("a second radio owner was admitted")
            assert_true(
                fcntl.fcntl(owner.fd, fcntl.F_GETFL) & os.O_ACCMODE == os.O_RDWR,
                "radio lease is not read/write",
            )

        real_parent = root / "real"
        real_parent.mkdir()
        symlink_parent = root / "linked"
        symlink_parent.symlink_to(real_parent, target_is_directory=True)
        try:
            with V1RadioLease(symlink_parent / "radio.lock", quiet_seconds=0):
                pass
        except RuntimeError:
            pass
        else:
            raise AssertionError("radio lease accepted a symlinked parent")


def test_runner_source_is_external_only_and_serial_is_read_only() -> None:
    source = (ROOT / "scripts" / "bench" / "run_window.py").read_text(encoding="utf-8")
    retired = (
        "Q" + "START",
        "Q" + "GETCSV",
        "Q" + "SYNC",
        "QB" + "SC08",
        "perf" + "_csv",
        "encounter" + "_csv",
        "display" + "_commits",
        "metric" + "_validation",
    )
    for name in retired:
        assert_true(name not in source, f"retired firmware evidence contract remains: {name}")
    bench_serial = source[source.index("class BenchSerial") : source.index("class V1Emulator")]
    assert_true(".ser.write(" not in bench_serial, "serial observer can still write to the device")
    assert_true('"evidence_contract": "external_only"' in source, "contract is not explicit")


GIT_SHA = "2f32ddab989792917b5b3df9206d9751ebfd8289"
RUNTIME_IDENTITY = {
    "boot_id": 42,
    "git_sha": "2f32dda",
    "image_id": "04904e028",
}


def build_upload_artifact(image_id: str, *, upload_performed: bool) -> dict[str, Any]:
    elf_sha = image_id + ("0" * (64 - len(image_id)))
    return {
        "upload_performed": upload_performed,
        "expected_runtime_image_id": image_id,
        "expected_runtime_image_id_basis": run_window_module.RUNTIME_IMAGE_ID_BASIS,
        "files": [{"name": "firmware.elf", "sha256": elf_sha}],
        "missing": [],
    }


def assert_identity_failure(call: Any, expected: str) -> RuntimeIdentityFailure:
    try:
        call()
    except RuntimeIdentityFailure as exc:
        assert_true(expected in str(exc), str(exc))
        return exc
    raise AssertionError(f"runtime identity failure was not raised: {expected}")


def test_upload_exact_match_is_qualified() -> None:
    result = qualify_runtime_identity(
        dict(RUNTIME_IDENTITY),
        intended_git_sha=GIT_SHA,
        build_upload=build_upload_artifact("04904e028", upload_performed=True),
        upload=True,
    )
    assert_true(result["status"] == "qualified", str(result))
    assert_true(result["git_match"] is True, str(result))
    assert_true(result["image_match"] is True, str(result))
    assert_true(result["artifact_linked"] is True, str(result))


def test_upload_git_mismatch_fails() -> None:
    identity = {**RUNTIME_IDENTITY, "git_sha": "38e02a8"}
    exc = assert_identity_failure(
        lambda: qualify_runtime_identity(
            identity,
            intended_git_sha=GIT_SHA,
            build_upload=build_upload_artifact("04904e028", upload_performed=True),
            upload=True,
        ),
        "does not match intended source commit",
    )
    assert_true(exc.qualification["git_match"] is False, str(exc.qualification))


def test_upload_image_mismatch_fails() -> None:
    exc = assert_identity_failure(
        lambda: qualify_runtime_identity(
            dict(RUNTIME_IDENTITY),
            intended_git_sha=GIT_SHA,
            build_upload=build_upload_artifact("111111111", upload_performed=True),
            upload=True,
        ),
        "does not match uploaded firmware image",
    )
    assert_true(exc.qualification["git_match"] is True, str(exc.qualification))
    assert_true(exc.qualification["image_match"] is False, str(exc.qualification))


def test_no_flash_git_match_with_linked_resident_artifact_is_qualified() -> None:
    result = qualify_runtime_identity(
        dict(RUNTIME_IDENTITY),
        intended_git_sha=GIT_SHA,
        build_upload=build_upload_artifact("04904e028", upload_performed=False),
        upload=False,
    )
    assert_true(result["status"] == "qualified", str(result))
    assert_true(result["mode"] == "no_flash", str(result))
    assert_true(result["artifact_linked"] is True, str(result))


def test_no_flash_git_match_with_unlinked_resident_artifact_is_collection_only() -> None:
    result = qualify_runtime_identity(
        dict(RUNTIME_IDENTITY),
        intended_git_sha=GIT_SHA,
        build_upload=build_upload_artifact("111111111", upload_performed=False),
        upload=False,
    )
    assert_true(result["status"] == "collection_only", str(result))
    assert_true(result["artifact_linked"] is False, str(result))
    assert_true("resident runtime image" in result["reason"], str(result))


def test_no_flash_git_mismatch_fails() -> None:
    identity = {**RUNTIME_IDENTITY, "git_sha": "38e02a8"}
    assert_identity_failure(
        lambda: qualify_runtime_identity(
            identity,
            intended_git_sha=GIT_SHA,
            build_upload=build_upload_artifact("04904e028", upload_performed=False),
            upload=False,
        ),
        "does not match intended source commit",
    )


def resident_fixture(root: Path, *, upload: bool = True) -> tuple[Path, Path, dict[str, Any], dict[str, Any]]:
    recording = root / "recorded"
    recording.mkdir()
    source_git = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    identity = {**RUNTIME_IDENTITY, "git_sha": source_git[:7]}
    manifest = build_upload_artifact(identity["image_id"], upload_performed=upload)
    manifest.update(schema_version=1, kind="bench_build_upload_artifacts")
    manifest["files"][0]["size_bytes"] = 1000
    app = bytearray(288)
    app[0:2] = bytes([0xE9, 1])
    struct.pack_into("<H", app, 12, 9)
    struct.pack_into("<II", app, 24, 0x3C000020, 256)
    struct.pack_into("<I", app, 32, 0xABCD5432)
    app[176:208] = bytes.fromhex(manifest["files"][0]["sha256"])
    image = root / "application.bin"
    image.write_bytes(app)
    manifest["files"].append({"name": "firmware.bin", "size_bytes": len(app),
                              "sha256": hashlib.sha256(app).hexdigest()})
    serial_path = recording / "bench_serial.log"
    serial_path.write_text(f"BOOT bootId=42 git={identity['git_sha']} image={identity['image_id']}\n")
    window = {"result": "PASS", "evidence_contract": "external_only", "git_sha": source_git,
              "git_ref": "main", "git_worktree_clean": True, "runtime_identity": identity,
              "runtime_qualification": qualify_runtime_identity(
                  identity, intended_git_sha=source_git, build_upload=manifest, upload=upload),
              "artifacts": {"bench_serial": file_artifact(serial_path)}}
    write_resident_fixture(recording, window, manifest)
    return recording, image, window, manifest


def write_resident_fixture(recording: Path, window: dict[str, Any], manifest: dict[str, Any]) -> None:
    path = recording / "build_upload_artifacts.json"
    path.write_text(json.dumps(manifest))
    window["artifacts"]["build_upload"] = {**file_artifact(path), **manifest}
    (recording / "window_result.json").write_text(json.dumps(window))


def test_resident_upload_reference_binds_original_bytes_and_fresh_identity() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        recording, image, window, _manifest = resident_fixture(root)
        output = root / "fresh"
        output.mkdir()
        result = run_window_module.retain_resident_artifacts(recording, image, output)
        provenance = result["resident_provenance"]
        assert_true(not result["upload_performed"], str(result))
        assert_true(provenance["source_git_sha"] == window["git_sha"], str(provenance))
        for name, record in provenance["reference_files"].items():
            original = image if name == "firmware.bin" else recording / name
            retained = output / record["path"]
            assert_true(retained.read_bytes() == original.read_bytes(), name)
            assert_true(record["sha256"] == hashlib.sha256(original.read_bytes()).hexdigest(), name)
        fresh = {**window["runtime_identity"], "boot_id": 43}
        qualified = qualify_runtime_identity(fresh, intended_git_sha=provenance["source_git_sha"],
                                            build_upload=result, upload=False)
        assert_true(qualified["status"] == "qualified", str(qualified))
        assert_identity_failure(lambda: qualify_runtime_identity(
            {**fresh, "git_sha": "1234567"}, intended_git_sha=provenance["source_git_sha"],
            build_upload=result, upload=False), "does not match intended source")
        wrong_image = qualify_runtime_identity(
            {**fresh, "image_id": "111111111"}, intended_git_sha=provenance["source_git_sha"],
            build_upload=result, upload=False)
        assert_true(wrong_image["status"] != "qualified", str(wrong_image))


def test_resident_upload_reference_rejects_unbound_evidence() -> None:
    for case, expected in (
        ("source", "source requires a full git"),
        ("source_mismatch", "does not match intended source"),
        ("dirty", "source was not clean"),
        ("mode", "upload flag does not match collection mode"),
        ("manifest", "reference bytes do not match build_upload"),
        ("embedded_manifest", "embedded upload manifest differs"),
        ("serial", "reference bytes do not match bench_serial"),
        ("serial_identity", "serial runtime identity differs"),
        ("binary", "application bytes do not match"),
        ("descriptor", "descriptor does not match"),
        ("descriptor_magic", "descriptor is missing"),
    ):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            recording, image, window, manifest = resident_fixture(root)
            if case == "source":
                window["git_sha"] = window["git_sha"][:7]
            elif case == "source_mismatch":
                window["runtime_identity"]["git_sha"] = "1234567"
                path = recording / "bench_serial.log"
                path.write_text(f"BOOT bootId=42 git=1234567 image={RUNTIME_IDENTITY['image_id']}\n")
                window["artifacts"]["bench_serial"] = file_artifact(path)
            elif case == "dirty":
                window["git_worktree_clean"] = False
            elif case == "mode":
                window["runtime_qualification"]["mode"] = "no_flash"
            elif case == "serial_identity":
                window["runtime_identity"]["boot_id"] = 99
            elif case in ("binary", "descriptor", "descriptor_magic"):
                changed = bytearray(image.read_bytes())
                changed[32 if case == "descriptor_magic" else 176] ^= 1
                image.write_bytes(changed)
                if case != "binary":
                    manifest["files"][1]["sha256"] = hashlib.sha256(changed).hexdigest()
            write_resident_fixture(recording, window, manifest)
            if case == "manifest":
                with (recording / "build_upload_artifacts.json").open("a") as handle:
                    handle.write(" ")
            elif case == "embedded_manifest":
                window["artifacts"]["build_upload"]["upload_performed"] = False
                (recording / "window_result.json").write_text(json.dumps(window))
            elif case == "serial":
                with (recording / "bench_serial.log").open("a") as handle:
                    handle.write("changed\n")
            output = root / "fresh"
            output.mkdir()
            assert_identity_failure(
                lambda: run_window_module.retain_resident_artifacts(recording, image, output), expected)
            assert_true(not list(output.iterdir()), "rejected provenance published reference files: " + case)


def test_resident_original_no_flash_reference_preserves_identity_without_upload_claim() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        recording, image, window, manifest = resident_fixture(root, upload=False)
        window["tooling_source"] = {"git_sha": window["git_sha"], "git_worktree_clean": True}
        write_resident_fixture(recording, window, manifest)
        output = root / "fresh"
        output.mkdir()
        result = run_window_module.retain_resident_artifacts(recording, image, output)
        provenance = result["resident_provenance"]
        assert_true(result["upload_performed"] is False, str(result))
        assert_true(provenance["kind"] == "qualified_prior_no_flash_application", str(provenance))
        assert_true(provenance["reference_collection_mode"] == "no_flash", str(provenance))
        assert_true("uploaded" not in provenance["application_binding"], str(provenance))
        assert_true(provenance["source_git_sha"] == window["git_sha"], str(provenance))
        qualified = qualify_runtime_identity({**window["runtime_identity"], "boot_id": 43},
            intended_git_sha=provenance["source_git_sha"], build_upload=result, upload=False)
        assert_true(qualified["status"] == "qualified" and qualified["mode"] == "no_flash", str(qualified))
        for name, record in provenance["reference_files"].items():
            original = image if name == "firmware.bin" else recording / name
            assert_true((output / record["path"]).read_bytes() == original.read_bytes(), name)


def test_resident_no_flash_reference_rejects_mode_drift_chaining_and_unbound_evidence() -> None:
    for case, expected in (("flag", "upload flag does not match"),
                           ("nonboolean_flag", "upload flag does not match"),
                           ("tooling", "tooling source differs"),
                           ("chain", "chained resident references"),
                           ("binary", "application bytes do not match"),
                           ("serial", "reference bytes do not match bench_serial"),
                           ("qualification", "qualification differs from its evidence")):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            recording, image, window, manifest = resident_fixture(root, upload=False)
            if case == "flag":
                manifest["upload_performed"] = True
            elif case == "nonboolean_flag":
                manifest["upload_performed"] = 0
            elif case == "tooling":
                window["tooling_source"] = {"git_sha": "1" * 40, "git_worktree_clean": True}
            elif case == "chain":
                manifest["resident_provenance"] = {"source_git_sha": window["git_sha"]}
            elif case == "qualification":
                window["runtime_qualification"]["artifact_image_id"] = "111111111"
            write_resident_fixture(recording, window, manifest)
            if case == "binary":
                image.write_bytes(image.read_bytes() + b"tampered")
            elif case == "serial":
                with (recording / "bench_serial.log").open("a") as handle:
                    handle.write("changed\n")
            output = root / "fresh"
            output.mkdir()
            assert_identity_failure(lambda: run_window_module.retain_resident_artifacts(recording, image, output), expected)
            assert_true(not list(output.iterdir()), "rejected reference published files: " + case)


def test_resident_reference_cli_requires_pair_without_upload() -> None:
    required = ["run_window.py", "--suite", "replay", "--out-dir", "unused"]
    for options in (("--resident-recording", "ref"), ("--resident-image", "app"),
                    ("--resident-recording", "ref", "--resident-image", "app", "--upload")):
        with mock.patch.object(sys, "argv", required + list(options)), contextlib.redirect_stderr(io.StringIO()):
            try:
                run_window_module.parse_args()
            except SystemExit as exc:
                assert_true(exc.code == 2, str(exc))
            else:
                raise AssertionError("invalid resident CLI accepted")
    for options in (("--replay", "--resident-recording", "ref", "--resident-image", "app"),
                    ("--replay", "--no-flash", "--resident-recording", "ref"),
                    ("--replay", "--no-flash", "--resident-image", "app"),
                    ("--analyze-recording", "old", "--no-flash", "--resident-recording", "ref",
                     "--resident-image", "app")):
        completed = subprocess.run([str(ROOT / "bench.sh"), *options], capture_output=True, text=True)
        assert_true(completed.returncode == 2 and "usage" in completed.stdout, completed.stdout)


def test_resident_collection_keeps_tooling_identity_separate_and_never_uploads() -> None:
    for fresh_change in ({}, {"git_sha": "1234567"}, {"image_id": "111111111"}):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            recording, image, window, _manifest = resident_fixture(root)
            output = root / "fresh"
            args = SimpleNamespace(
                board_id="fixture", blink_arrow=False, blink_profile="steady",
                out_dir=str(output), runner_stdout_log="", runner_stderr_log="",
                duration_seconds=1, ready_timeout_seconds=1, post_upload_settle_seconds=0,
                suite="core", scenario="", reader_qualification=False,
                replay_executable="fixture-replay", git_sha="f" * 40,
                git_ref="newer-tooling", git_worktree_clean="1", upload=False,
                resident_recording=str(recording), resident_image=str(image),
                camera=False, port="fixture-port", baud=115200,
            )
            fresh = {**window["runtime_identity"], "boot_id": 43, **fresh_change}
            observer = mock.Mock(runtime_identity=fresh, boot_marker_count=1, line_count=1)
            emulator = mock.Mock()
            emulator.finish.return_value = {"lifecycle_completed": True}
            lease = mock.MagicMock()
            lease.__enter__.return_value.fd = 1
            with contextlib.ExitStack() as stack:
                for name, value in (
                    ("parse_args", lambda: args), ("install_signal_handlers", lambda: None),
                    ("serial", object()), ("V1RadioLease", lambda: lease),
                    ("wait_for_port", lambda *_: "fixture-port"),
                    ("BenchSerial", lambda *_: observer), ("V1Emulator", lambda *a, **k: emulator),
                    ("establish_serial_boundary", lambda *a, **k: None),
                ):
                    stack.enter_context(mock.patch.object(run_window_module, name, value))
                upload = stack.enter_context(mock.patch.object(run_window_module, "run_upload"))
                local_artifact = stack.enter_context(mock.patch.object(
                    run_window_module, "retain_build_upload_artifacts"))
                stack.enter_context(mock.patch.object(
                    run_window_module.time, "monotonic", side_effect=itertools.count(0, 2)))
                stack.enter_context(contextlib.redirect_stderr(io.StringIO()))
                status = run_window_module.main()
            payload = json.loads((output / "window_result.json").read_text())
            upload.assert_not_called()
            local_artifact.assert_not_called()
            observer.reset_for_boot.assert_called_once()
            if fresh_change:
                assert_true(status == 3 and payload["failure_kind"] == "runtime_identity", str(payload))
                emulator.start.assert_not_called()
            else:
                assert_true(status == 0 and payload["result"] == "PASS", str(payload))
                assert_true(payload["git_sha"] == window["git_sha"] != args.git_sha, str(payload))
                assert_true(payload["git_ref"] == "main", str(payload))
                assert_true(payload["tooling_source"] == {
                    "git_sha": args.git_sha, "git_ref": args.git_ref, "git_worktree_clean": True,
                }, str(payload))
                assert_true(payload["runtime_identity"] == fresh, str(payload))
                assert_true(payload["runtime_qualification"]["mode"] == "no_flash", str(payload))
                emulator.start.assert_called_once()
                emulator.finish.assert_called_once_with(True)


def test_main_writes_collection_only_and_returns_exit_one_for_unlinked_no_flash() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        out_dir = Path(tmp)
        args = SimpleNamespace(
            camera=False,
            board_id="fixture",
            blink_arrow=False,
            blink_profile="steady",
            out_dir=str(out_dir),
            runner_stdout_log="",
            runner_stderr_log="",
            duration_seconds=1,
            ready_timeout_seconds=1,
            post_upload_settle_seconds=0,
            suite="core",
            scenario="",
            reader_qualification=False,
            replay_executable="fixture-replay",
            git_sha=GIT_SHA,
            git_ref="main",
            git_worktree_clean="1",
        )
        reason = "resident runtime image 04904e028 is not linked to the retained firmware ELF"
        collected = {
            "port": "fixture-port",
            "completion": {},
            "emulator": {},
            "camera": {},
            "runtime_identity": dict(RUNTIME_IDENTITY),
            "runtime_qualification": {
                "status": "collection_only",
                "mode": "no_flash",
                "git_match": True,
                "artifact_linked": False,
                "image_match": False,
                "reason": reason,
            },
        }
        originals = {
            "parse_args": run_window_module.parse_args,
            "install_signal_handlers": run_window_module.install_signal_handlers,
            "collect_live": run_window_module.collect_live,
            "serial": run_window_module.serial,
        }
        run_window_module.parse_args = lambda: args  # type: ignore[assignment]
        run_window_module.install_signal_handlers = lambda: None  # type: ignore[assignment]
        run_window_module.collect_live = (  # type: ignore[assignment]
            lambda _args, _out_dir, _artifacts: collected
        )
        run_window_module.serial = object()  # type: ignore[assignment]
        stderr = io.StringIO()
        try:
            with contextlib.redirect_stderr(stderr):
                status = run_window_module.main()
        finally:
            for name, value in originals.items():
                setattr(run_window_module, name, value)

        payload = json.loads((out_dir / "window_result.json").read_text(encoding="utf-8"))
        assert_true(status == 1, f"collection-only exit changed: {status}")
        assert_true(payload["result"] == "COLLECTION_ONLY", str(payload))
        assert_true(payload["qualification_reason"] == reason, str(payload))
        assert_true(payload["runtime_qualification"] == collected["runtime_qualification"], str(payload))
        assert_true("collection_only" in stderr.getvalue(), stderr.getvalue())


def test_dirty_source_vetoes_qualification_before_collection() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        out_dir = Path(tmp)
        args = SimpleNamespace(
            camera=False,
            board_id="fixture",
            blink_arrow=False,
            blink_profile="steady",
            out_dir=str(out_dir),
            runner_stdout_log="",
            runner_stderr_log="",
            duration_seconds=1,
            ready_timeout_seconds=1,
            post_upload_settle_seconds=0,
            suite="core",
            scenario="",
            reader_qualification=False,
            replay_executable="fixture-replay",
            git_sha=GIT_SHA,
            git_ref="main",
            git_worktree_clean="0",
        )
        collected = False
        originals = {
            "parse_args": run_window_module.parse_args,
            "install_signal_handlers": run_window_module.install_signal_handlers,
            "collect_live": run_window_module.collect_live,
        }

        def unexpected_collection(*_args: Any) -> dict[str, Any]:
            nonlocal collected
            collected = True
            raise AssertionError("dirty source reached live collection")

        run_window_module.parse_args = lambda: args  # type: ignore[assignment]
        run_window_module.install_signal_handlers = lambda: None  # type: ignore[assignment]
        run_window_module.collect_live = unexpected_collection  # type: ignore[assignment]
        try:
            status = run_window_module.main()
        finally:
            for name, value in originals.items():
                setattr(run_window_module, name, value)

        payload = json.loads((out_dir / "window_result.json").read_text(encoding="utf-8"))
        assert_true(status == 2, f"dirty qualification exit changed: {status}")
        assert_true(collected is False, "dirty source began live collection")
        assert_true(payload["result"] == "FAIL", str(payload))
        assert_true(payload["failure_kind"] == "source_provenance", str(payload))
        assert_true(payload["runtime_qualification"]["status"] == "unqualified", str(payload))


def run_replay_delivery_verdict(
    delivery: dict[str, Any],
) -> tuple[int, dict[str, Any]]:
    with tempfile.TemporaryDirectory() as tmp:
        out_dir = Path(tmp)
        args = SimpleNamespace(
            camera=False,
            board_id="fixture",
            blink_arrow=False,
            blink_profile="scenario",
            out_dir=str(out_dir),
            runner_stdout_log="",
            runner_stderr_log="",
            duration_seconds=1,
            ready_timeout_seconds=1,
            post_upload_settle_seconds=0,
            suite="replay",
            scenario="fixture.json",
            reader_qualification=False,
            replay_executable="fixture-replay",
            git_sha=GIT_SHA,
            git_ref="main",
            git_worktree_clean="1",
        )
        collected = {
            "port": "fixture-port",
            "completion": {},
            "emulator": {"notification_delivery": delivery},
            "camera": {},
            "runtime_identity": dict(RUNTIME_IDENTITY),
            "runtime_qualification": {"status": "qualified"},
        }
        originals = {
            "parse_args": run_window_module.parse_args,
            "install_signal_handlers": run_window_module.install_signal_handlers,
            "collect_live": run_window_module.collect_live,
            "serial": run_window_module.serial,
        }
        run_window_module.parse_args = lambda: args  # type: ignore[assignment]
        run_window_module.install_signal_handlers = lambda: None  # type: ignore[assignment]
        run_window_module.collect_live = (  # type: ignore[assignment]
            lambda _args, _out_dir, _artifacts: collected
        )
        run_window_module.serial = object()  # type: ignore[assignment]
        try:
            status = run_window_module.main()
        finally:
            for name, value in originals.items():
                setattr(run_window_module, name, value)

        payload = json.loads((out_dir / "window_result.json").read_text(encoding="utf-8"))
        return status, payload


def test_top_level_pass_is_vetoed_by_delivery_loss_counters() -> None:
    delivery = summarize_notification_delivery(
        [
            {"state": "notification_requested", "globalTxSequence": 1},
            {"state": "notification_dropped", "globalTxSequence": 1},
        ]
    )
    status, payload = run_replay_delivery_verdict(delivery)
    assert_true(status == 2, f"delivery-loss exit changed: {status}")
    assert_true(payload["result"] == "FAIL", str(payload))
    assert_true(payload["failure_kind"] == "replay_delivery", str(payload))
    assert_true("dropped=1" in payload["qualification_reason"], str(payload))


def test_top_level_pass_is_vetoed_by_malformed_delivery_sequence() -> None:
    delivery = summarize_notification_delivery(
        [
            {"state": "notification_requested", "globalTxSequence": 1},
            {"state": "notification_accepted", "globalTxSequence": 1},
            {"state": "notification_accepted", "globalTxSequence": 1},
        ]
    )
    status, payload = run_replay_delivery_verdict(delivery)
    assert_true(status == 2, f"malformed delivery exit changed: {status}")
    assert_true(payload["result"] == "FAIL", str(payload))
    assert_true(payload["failure_kind"] == "replay_delivery", str(payload))
    assert_true("malformed_sequences=1" in payload["qualification_reason"], str(payload))


def test_top_level_pass_is_vetoed_by_empty_delivery_stream() -> None:
    status, payload = run_replay_delivery_verdict(summarize_notification_delivery([]))
    assert_true(status == 2, f"empty delivery exit changed: {status}")
    assert_true(payload["result"] == "FAIL", str(payload))
    assert_true(payload["failure_kind"] == "replay_delivery", str(payload))
    assert_true("evidence is empty" in payload["qualification_reason"], str(payload))


def test_clean_source_preserves_qualified_pass_behavior() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        out_dir = Path(tmp)
        args = SimpleNamespace(
            camera=False,
            board_id="fixture",
            blink_arrow=False,
            blink_profile="steady",
            out_dir=str(out_dir),
            runner_stdout_log="",
            runner_stderr_log="",
            duration_seconds=1,
            ready_timeout_seconds=1,
            post_upload_settle_seconds=0,
            suite="core",
            scenario="",
            reader_qualification=False,
            replay_executable="fixture-replay",
            git_sha=GIT_SHA,
            git_ref="main",
            git_worktree_clean="1",
        )
        collected = {
            "port": "fixture-port",
            "completion": {},
            "emulator": {},
            "camera": {},
            "runtime_identity": dict(RUNTIME_IDENTITY),
            "runtime_qualification": {"status": "qualified"},
        }
        originals = {
            "parse_args": run_window_module.parse_args,
            "install_signal_handlers": run_window_module.install_signal_handlers,
            "collect_live": run_window_module.collect_live,
            "serial": run_window_module.serial,
        }
        run_window_module.parse_args = lambda: args  # type: ignore[assignment]
        run_window_module.install_signal_handlers = lambda: None  # type: ignore[assignment]
        run_window_module.collect_live = (  # type: ignore[assignment]
            lambda _args, _out_dir, _artifacts: collected
        )
        run_window_module.serial = object()  # type: ignore[assignment]
        try:
            status = run_window_module.main()
        finally:
            for name, value in originals.items():
                setattr(run_window_module, name, value)

        payload = json.loads((out_dir / "window_result.json").read_text(encoding="utf-8"))
        assert_true(status == 0, f"clean qualified exit changed: {status}")
        assert_true(payload["result"] == "PASS", str(payload))


def test_bench_cli_collection_only_branch_has_no_pass_verdict() -> None:
    source = (ROOT / "bench.sh").read_text(encoding="utf-8")
    dirty_veto = source.index('if [[ "$GIT_WORKTREE_CLEAN" -ne 1 ]]; then')
    device_detection = source.index('PORT="$(detect_usb_port || true)"')
    assert_true(dirty_veto < device_detection, "dirty source can still reach a partial PASS")
    start = source.index('if [[ "$COLLECTION_ONLY" -eq 1 ]]; then')
    end = source.index("\nfi", start) + len("\nfi")
    branch = source[start:end]
    assert_true(
        'finish "COLLECTION-ONLY (unqualified: $COLLECTION_ONLY_REASON)" 1' in branch,
        branch,
    )
    assert_true("PASS" not in branch, branch)


def run_bench_cli_fixture(window_result: str, counter_result: str, *,
                          encounter_result: str = "NO_DIFFERENCES_OBSERVED", camera: bool = True,
                          camera_present: bool = True, visual_exit: int = 0,
                          encounter_exit: int | None = None,
                          encounter_payload: dict | None = None,
                          write_encounter_result: bool = True,
                          interrupt_encounter: bool = False,
                          run_all: bool = False,
                          offline: bool = False,
                          compare: bool = False,
                          reuse: bool = False,
                          analysis_ranges: tuple[str, ...] = (),
                          extra_arguments: tuple[str, ...] = (),
                          qualification_capture: bool = False,
                          capture_records: list[dict | None] | None = None,
                          reader_environment_ok: bool = True,
                          environment_records: list[dict] | None = None,
                          analysis_logs: list[str] | None = None,
                          ) -> tuple[subprocess.CompletedProcess[str], int, int, int]:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        bench = root / "bench.sh"
        bench.write_bytes((ROOT / "bench.sh").read_bytes())
        bench.chmod(0o755)
        fake_bin = root / "bin"
        fake_bin.mkdir()

        git = fake_bin / "git"
        git.write_text(textwrap.dedent("""\
            #!/usr/bin/env bash
            if [[ "$1" == "status" ]]; then exit 0; fi
            if [[ "$1" == "rev-parse" && "$2" == "--short" ]]; then printf '0123456\\n'; exit 0; fi
            if [[ "$1" == "rev-parse" && "$2" == "--abbrev-ref" ]]; then printf 'main\\n'; exit 0; fi
            if [[ "$1" == "rev-parse" ]]; then printf '0123456789abcdef0123456789abcdef01234567\\n'; exit 0; fi
            exit 1
        """), encoding="utf-8")
        git.chmod(0o755)
        profiler = fake_bin / "system_profiler"
        profiler.write_text("#!/bin/sh\nprintf '" + ("Global Shutter Camera" if camera_present else "none") + "\\n'\n", encoding="utf-8")
        profiler.chmod(0o755)
        xcrun = fake_bin / "xcrun"
        xcrun.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
        xcrun.chmod(0o755)

        window_status = {"PASS": 0, "COLLECTION_ONLY": 1, "FAIL": 2}[window_result]
        window = {
            "result": window_result,
            "failure_kind": "collection" if window_result == "FAIL" else "none",
            "qualification_reason": "fixture runtime artifact is unlinked" if window_result == "COLLECTION_ONLY" else None,
            "error": "fixture collection failed" if window_result == "FAIL" else None,
            "completion": {"duration_seconds": 1, "serial_lines_observed": 4},
            "runtime_identity": {"git_sha": "0123456789abcdef0123456789abcdef01234567", "image_id": "123456789"},
            "runtime_qualification": {"mode": "fixture", "status": "collection_only" if window_result == "COLLECTION_ONLY" else "qualified"},
            "emulator": {"notification_delivery": {"requested": 10, "delivered": 10, "dropped": 0, "skipped": 0}},
            "camera": ({"result": "CAPTURED", "recorder_stats": {"frames_appended": 200,
                        "capture_drops": 0, "writer_backpressure_drops": 0},
                        "video_probe": {"average_frame_rate": 200.0}} if camera else None),
        }
        counter_counts = {
            "PASS": {"matched": 2, "mismatched": 0, "unresolved": 0, "required": 2},
            "FAIL": {"matched": 1, "mismatched": 1, "unresolved": 0, "required": 2},
            "INCONCLUSIVE": {"matched": 1, "mismatched": 0, "unresolved": 1, "required": 2},
        }[counter_result]
        window_path = root / "window.json"
        counter_path = root / "counter.json"
        window_path.write_text(json.dumps(window), encoding="utf-8")
        counter_path.write_text(json.dumps({"result": counter_result, "counts": counter_counts}), encoding="utf-8")
        encounter_path = root / "encounter.json"
        encounter = encounter_payload if encounter_payload is not None else generated_encounter_payload((encounter_result,))
        encounter_path.write_text(json.dumps(encounter), encoding="utf-8")
        counter_marker = root / "counter.calls"
        encounter_marker = root / "encounter.calls"
        visual_marker = root / "visual.calls"

        runtime = root / "runtime"
        runtime.mkdir()
        python = runtime / "python3"
        python.write_text(textwrap.dedent(f"""\
            #!/usr/bin/env bash
            set -uo pipefail
            fixture_root="$(cd "$(dirname "$0")/.." && pwd)"
            if [[ "${{1:-}}" == "-c" ]]; then printf 'fixture\\n'; exit 0; fi
            if [[ "${{1:-}}" == "-" ]]; then exec {sys.executable} "$@"; fi
            if [[ "${{1:-}}" == */scripts/bench/run_logged.py ]]; then
              if [[ "$FAKE_OFFLINE" == 1 ]]; then
                printf 'hardware called\\n' >> "$FAKE_HARDWARE_MARKER"
                exit 97
              fi
              if [[ " $* " == *"/tools/v1replay/scripts/build.sh"* ]]; then
                printf 'called\\n' >> "$FAKE_BUILD_MARKER"
                mkdir -p "$fixture_root/tools/v1replay/.build"
                printf '#!/bin/sh\\nexit 0\\n' > "$fixture_root/tools/v1replay/.build/v1replay"
                chmod +x "$fixture_root/tools/v1replay/.build/v1replay"
                exit 0
              fi
              if [[ " $* " == *"/scripts/bench/run_window.py"* ]]; then
                printf 'called\\n' >> "$FAKE_WINDOW_MARKER"
                args=("$@")
                reader_qualification=0
                for ((index=0; index<${{#args[@]}}; index++)); do
                  if [[ "${{args[index]}}" == "--out-dir" ]]; then out="${{args[index+1]}}"; fi
                  if [[ "${{args[index]}}" == "--reader-qualification" ]]; then reader_qualification=1; fi
                done
                [[ "$reader_qualification" == "{int(qualification_capture)}" ]] || exit 12
                if [[ "{int(camera)}" == 1 ]]; then
                  printf '[bench] frequency reading: Calibrated frequency fallback unavailable: counter_edges (before collection)\\n'
                fi
                mkdir -p "$out"
                cp "$FAKE_WINDOW_JSON" "$out/window_result.json"
                exit "$FAKE_WINDOW_EXIT"
              fi
            fi
            if [[ "${{1:-}}" == */scripts/bench/counter_check.py ]]; then
              args=("$@")
              for ((index=0; index<${{#args[@]}}; index++)); do
                if [[ "${{args[index]}}" == "--out" ]]; then out="${{args[index+1]}}"; fi
              done
              [[ ! -e "$out" ]] || exit 9
              mkdir -p "$out"
              cp "$FAKE_COUNTER_JSON" "$out/result.json"
              printf 'called\\n' >> "$FAKE_COUNTER_MARKER"
              exit "$FAKE_COUNTER_EXIT"
            fi
            if [[ "${{1:-}}" == */scripts/bench/encounter_check.py ]]; then
              args=("$@")
              behavior_review=0
              comparison=""
              reading_reuse=""
              reader_qualification=0
              for ((index=0; index<${{#args[@]}}; index++)); do
                if [[ "${{args[index]}}" == "--range" ]]; then
                  printf '%s\\n' "${{args[index+1]}}" >> "$FAKE_RANGE_MARKER"
                fi
                if [[ "${{args[index]}}" == "--out" ]]; then out="${{args[index+1]}}"; fi
                if [[ "${{args[index]}}" == "--run-dir" ]]; then run="${{args[index+1]}}"; fi
                if [[ "${{args[index]}}" == "--observe-behavior" ]]; then behavior_review=1; fi
                if [[ "${{args[index]}}" == "--compare-to" ]]; then comparison="${{args[index+1]}}"; fi
                if [[ "${{args[index]}}" == "--reuse-readings" ]]; then reading_reuse="${{args[index+1]}}"; fi
                if [[ "${{args[index]}}" == "--reader-qualification" ]]; then reader_qualification=1; fi
              done
              [[ "$behavior_review" == 1 && "$reader_qualification" == 1 && "$comparison" == "$FAKE_COMPARE_TO" && "$reading_reuse" == "$FAKE_REUSE_READINGS" ]] || exit 9
              [[ -e "$run/window_result.json" && ! -e "$out" ]] || exit 9
              mkdir -p "$out"
              if [[ "$FAKE_ENCOUNTER_WRITE" == 1 ]]; then
                cp "$FAKE_ENCOUNTER_JSON" "$out/result.json"
                printf '<title>fixture encounter report</title>\\n' > "$out/report.html"
              fi
              printf 'called\\n' >> "$FAKE_ENCOUNTER_MARKER"
              printf 'Reader diagnostic: retained in bench.log\\n'
              printf '[bench] frequency reading: Calibrated frequency fallback unavailable: counter_edges\\n'
              for ((frame=1; frame<=10001; frame+=500)); do
                printf 'Read %s/10001 original event frames\\n' "$frame"
              done
              printf 'Standalone analyzer summary: retained in bench.log\\n'
              if [[ "$FAKE_ENCOUNTER_INTERRUPT" == 1 ]]; then kill -TERM "$PPID"; fi
              exit "$FAKE_ENCOUNTER_EXIT"
            fi
            if [[ "${{1:-}}" == */scripts/bench/visual_run_check.py ]]; then
              printf 'called\\n' >> "$FAKE_VISUAL_MARKER"
              exit "$FAKE_VISUAL_EXIT"
            fi
            exec {sys.executable} "$@"
        """), encoding="utf-8")
        python.chmod(0o755)
        ambient_python = fake_bin / "python3"
        ambient_python.write_text(
            '#!/bin/sh\nprintf "called\\n" >> "$FAKE_AMBIENT_PYTHON_MARKER"\nexit 97\n',
            encoding="utf-8")
        ambient_python.chmod(0o755)
        bootstrap = root / "scripts" / "bench_python.sh"
        bootstrap.parent.mkdir()
        bootstrap.write_text(textwrap.dedent("""\
            #!/usr/bin/env bash
            printf '%s\\n' "${1:-}" >> "$FAKE_BOOTSTRAP_MARKER"
            if [[ "$FAKE_READER_ENVIRONMENT_OK" != 1 ]]; then
              printf 'reader environment differs: numpy_version expected 2.5.1, found 2.5.2\\n' >&2
              exit 2
            fi
            printf '%s\\n' "$FAKE_BENCH_PYTHON"
        """), encoding="utf-8")
        bootstrap.chmod(0o755)
        if "persistence" in encounter:
            # The terminal validator recomputes the actual adapter, even when
            # acquisition itself is replaced by this no-hardware fixture.
            modules = root / "scripts" / "bench"
            modules.mkdir()
            for name in ("encounter_persistence.py", "encounter_expectation.py", "counter_expectation.py"):
                (modules / name).write_bytes((ROOT / "scripts" / "bench" / name).read_bytes())
        device = root / "device"
        if not offline:
            device.touch()
        hardware_marker = root / "hardware.calls"
        build_marker = root / "build.calls"
        window_marker = root / "window.calls"
        bootstrap_marker = root / "bootstrap.calls"
        ambient_python_marker = root / "ambient-python.calls"
        range_marker = root / "ranges.calls"
        recorded = root / "recorded"
        recorded.mkdir()
        (recorded / "window_result.json").write_bytes(window_path.read_bytes())
        if offline:
            profiler.write_text("#!/bin/sh\nprintf 'called\\n' >> \"$FAKE_HARDWARE_MARKER\"\nexit 97\n", encoding="utf-8")
        baseline_path = root / "baseline.json"
        baseline_path.write_text(json.dumps(encounter), encoding="utf-8")
        environment = dict(os.environ)
        environment.update(
            PATH=str(fake_bin) + os.pathsep + environment.get("PATH", ""),
            DEVICE_PORT=str(device),
            BENCH_ARTIFACT_ROOT=str(root / "artifacts"),
            BENCH_DURATION_SECONDS="1",
            BENCH_REPLAY_DURATION_SECONDS="1",
            BENCH_POST_UPLOAD_SETTLE_SECONDS="0",
            FAKE_OFFLINE=str(int(offline)),
            FAKE_READER_ENVIRONMENT_OK=str(int(reader_environment_ok)),
            FAKE_BENCH_PYTHON=str(python),
            FAKE_BOOTSTRAP_MARKER=str(bootstrap_marker),
            FAKE_AMBIENT_PYTHON_MARKER=str(ambient_python_marker),
            FAKE_BUILD_MARKER=str(build_marker),
            FAKE_WINDOW_MARKER=str(window_marker),
            FAKE_COMPARE_TO=str(baseline_path) if compare else "",
            FAKE_REUSE_READINGS=str(baseline_path) if reuse else "",
            FAKE_HARDWARE_MARKER=str(hardware_marker),
            FAKE_RANGE_MARKER=str(range_marker),
            FAKE_WINDOW_JSON=str(window_path),
            FAKE_WINDOW_EXIT=str(window_status),
            FAKE_COUNTER_JSON=str(counter_path),
            FAKE_COUNTER_EXIT=str({"PASS": 0, "FAIL": 1, "INCONCLUSIVE": 2}[counter_result]),
            FAKE_COUNTER_MARKER=str(counter_marker),
            FAKE_ENCOUNTER_JSON=str(encounter_path),
            FAKE_ENCOUNTER_EXIT=str(encounter_exit if encounter_exit is not None else {"NO_DIFFERENCES_OBSERVED": 0, "DIFFERENCES_FOUND": 1, "MEASUREMENT_INCOMPLETE": 2}[encounter_result]),
            FAKE_ENCOUNTER_MARKER=str(encounter_marker),
            FAKE_ENCOUNTER_WRITE=str(int(write_encounter_result)),
            FAKE_ENCOUNTER_INTERRUPT=str(int(interrupt_encounter)),
            FAKE_VISUAL_MARKER=str(visual_marker),
            FAKE_VISUAL_EXIT=str(visual_exit),
        )
        arguments = ([str(bench), "--analyze-recording", str(recorded)] if offline else
                     [str(bench), "--all" if run_all else "--replay", "--no-flash"])
        if compare:
            arguments.extend(("--compare-to", str(baseline_path)))
        if reuse:
            arguments.extend(("--reuse-readings", str(baseline_path)))
        for value in analysis_ranges:
            arguments.extend(("--range", value))
        arguments.extend(extra_arguments)
        if camera and not offline:
            arguments.append("--camera")
        if qualification_capture:
            arguments.append("--qualification-capture")
        process = subprocess.run(arguments, cwd=root, env=environment, capture_output=True, text=True)
        if offline:
            assert_true(not hardware_marker.exists(), "offline analysis called hardware")
            assert_true(sorted(child.name for child in recorded.iterdir()) == ["window_result.json"],
                        "offline analysis wrote into the retained recording")
            assert_true((recorded / "window_result.json").read_bytes() == window_path.read_bytes(),
                        "offline analysis changed retained collection evidence")
            if encounter_marker.exists():
                outputs = list((root / "artifacts").glob("*/runs/*/encounter-check/result.json"))
                assert_true(len(outputs) == 1, "offline output is not in a new ordinary run directory")
                ranges = range_marker.read_text().splitlines() if range_marker.exists() else []
                assert_true(ranges == list(analysis_ranges), "offline ranges were not forwarded exactly")
        counter_calls = len(counter_marker.read_text().splitlines()) if counter_marker.exists() else 0
        encounter_calls = len(encounter_marker.read_text().splitlines()) if encounter_marker.exists() else 0
        visual_calls = len(visual_marker.read_text().splitlines()) if visual_marker.exists() else 0
        if environment_records is not None:
            environment_records.append({
                "qualification_arguments": bootstrap_marker.read_text().splitlines() if bootstrap_marker.exists() else [],
                "build_calls": len(build_marker.read_text().splitlines()) if build_marker.exists() else 0,
                "window_calls": len(window_marker.read_text().splitlines()) if window_marker.exists() else 0,
                "ambient_python_calls": len(ambient_python_marker.read_text().splitlines()) if ambient_python_marker.exists() else 0,
                "hardware_calls": len(hardware_marker.read_text().splitlines()) if hardware_marker.exists() else 0,
            })
        if capture_records is not None:
            records = list((root / "artifacts").glob("*/runs/*/replay/qualification_capture.json"))
            capture_records.append(json.loads(records[0].read_text(encoding="utf-8")) if len(records) == 1 else None)
        if analysis_logs is not None:
            analysis_logs.extend(path.read_text() for path in (root / "artifacts").glob("*/runs/*/bench.log"))
        return process, counter_calls, encounter_calls, visual_calls


def test_bench_cli_uses_selected_reader_environment_and_rejects_before_work() -> None:
    for offline in (False, True):
        records: list[dict] = []
        process, counter_calls, encounter_calls, _ = run_bench_cli_fixture(
            "PASS", "PASS", offline=offline, environment_records=records)
        assert_true(process.returncode == 0, process.stdout + process.stderr)
        assert_true((counter_calls, encounter_calls) == (0 if offline else 1, 1), process.stdout)
        selected = records[0]
        assert_true(len(selected["qualification_arguments"]) == 1
                    and selected["qualification_arguments"][0].endswith("/qualification/encounter-reader.json"),
                    str(selected))
        assert_true(selected["ambient_python_calls"] == 0,
                    "bench escaped its selected reader environment: " + str(selected))
        assert_true(selected["build_calls"] == selected["window_calls"] == (0 if offline else 1), str(selected))

        records.clear()
        process, counter_calls, encounter_calls, visual_calls = run_bench_cli_fixture(
            "PASS", "PASS", offline=offline, reader_environment_ok=False,
            environment_records=records)
        assert_true(process.returncode == 2, process.stdout + process.stderr)
        assert_true("MEASUREMENT_INCOMPLETE (reader environment)" in process.stdout,
                    process.stdout + process.stderr)
        assert_true("numpy_version expected 2.5.1, found 2.5.2" in process.stderr,
                    process.stdout + process.stderr)
        assert_true((counter_calls, encounter_calls, visual_calls) == (0, 0, 0), process.stdout)
        rejected = records[0]
        assert_true(len(rejected["qualification_arguments"]) == 1, str(rejected))
        assert_true(all(rejected[key] == 0 for key in (
            "build_calls", "window_calls", "ambient_python_calls", "hardware_calls")), str(rejected))


def generated_encounter_payload(outcomes: tuple[str, ...]) -> dict:
    """Use the real observation adapter for the behavior-entrypoint contract."""
    from bench.encounter_observation import summarize_event_observations
    from encounter_behavior import event_findings, summarize
    fields = ("counter_glyph", "primary_frequency", "active_bands", "main_arrows",
              "main_bars", "secondary", "muted_badge")
    events = []
    for number, outcome in enumerate(outcomes, 1):
        first = {"frame_id": f"{number:04d}-1", "capture_ns": 10_000_000, "image": "frames/1.png"}
        last = {"frame_id": f"{number:04d}-2", "capture_ns": 20_000_000, "image": "frames/2.png"}
        different = outcome == "DIFFERENCES_FOUND"
        unknown = outcome in ("MEASUREMENT_INCOMPLETE", "TRANSIENT_UNKNOWN")
        missing_target = outcome == "MEASUREMENT_INCOMPLETE"
        checks = {name: "MATCH" for name in fields}
        checks["primary_frequency"] = "DIFFERENCE" if different else "UNRESOLVED" if unknown else "MATCH"
        target = {"fields": {name: {"allowed": [1]} for name in fields}}
        coverage = {"available_recorded_frames": 2, "read_recorded_frames": 2,
                    "complete_recorded_frame_coverage": True, "unrecorded_source_frames": 0}
        sequence_event = {"event_id": f"event-{number:04d}", "start_ns": 0, "end_ns": 1_000_000_000,
                          "target_basis": {"first_complete_target_input_ns": 0}, "target": target,
                          "first_correct": None if missing_target else first, "coverage": coverage,
                          "observation_spans": [{"first": first, "last": last, "frame_count": 2,
                             "judgment": {"status": "NOT_CORRECT" if different else "UNRESOLVED" if unknown else "CORRECT",
                                          "fields": checks},
                             "observed": {name: {"state": "ambiguous" if unknown and name == "primary_frequency" else "readable",
                                                  "value": 2 if different and name == "primary_frequency" else 1} for name in fields}}]}
        if not missing_target and (different or unknown):
            from copy import deepcopy
            first_span = deepcopy(sequence_event["observation_spans"][0])
            first_span.update(last=first, frame_count=1)
            first_span["judgment"].update(status="CORRECT", fields={name: "MATCH" for name in fields})
            first_span["observed"] = {name: {"state": "readable", "value": 1} for name in fields}
            second_span = sequence_event["observation_spans"][0]
            second_span.update(first=last, frame_count=1)
            sequence_event["observation_spans"] = [first_span, second_span]
        observation = summarize_event_observations(sequence_event)
        observation["first_target_ms"] = None if missing_target else 10
        findings = event_findings(sequence_event, observation, {"field_rule_ids": {}})
        events.append({"event_id": sequence_event["event_id"], "input_key": [number], "target": target,
                       "observation": observation, "findings": findings, "coverage": coverage,
                       "unresolved_frames": 2 if missing_target else 1 if unknown else 0})
    qualification = {"status": "QUALIFIED"}
    result, summary = summarize(events, [], qualification)
    return {"schema_version": 1, "kind": "firmware_visual_behavior", "errors": [],
            "result": result, "reader_qualification": qualification,
            "evidence": {"runtime_identity": {"git_sha": "0123456", "image_id": "123456789"}},
            "events": events, "summary": summary}


def test_bench_cli_consumes_current_producer_results_online_and_offline() -> None:
    for outcomes in (("NO_DIFFERENCES_OBSERVED",), ("DIFFERENCES_FOUND",), ("MEASUREMENT_INCOMPLETE",),
                     ("NO_DIFFERENCES_OBSERVED", "DIFFERENCES_FOUND", "MEASUREMENT_INCOMPLETE")):
        payload = generated_encounter_payload(outcomes)
        result, counts = payload["result"], payload["summary"]
        expected = (f"target observed {counts['targets_observed']}/{counts['events']} events | "
                    f"findings {counts['findings']} | affected events {counts['events_with_findings']}")
        for offline in (False, True):
            process, counter_calls, encounter_calls, _ = run_bench_cli_fixture(
                "PASS", "PASS", encounter_result=result, encounter_payload=payload,
                offline=offline, analysis_ranges=("4.99:5.40", "8.99:9.34") if offline else ())
            assert_true(process.returncode == {"NO_DIFFERENCES_OBSERVED": 0, "DIFFERENCES_FOUND": 1, "MEASUREMENT_INCOMPLETE": 2}[result],
                        process.stdout + process.stderr)
            assert_true(expected in process.stdout, process.stdout)
            assert_true(encounter_calls == 1 and counter_calls == int(not offline), process.stdout)
            assert_true("100 ms" not in process.stdout, process.stdout)
            if offline:
                assert_true("OFFLINE recorded firmware analysis" in process.stdout, process.stdout)
                assert_true("OFFLINE recorded runtime: git " in process.stdout, process.stdout)
                assert_true(process.stdout.splitlines()[-1] == f"{result} (OFFLINE recorded visual behavior)", process.stdout)


def test_bench_cli_keeps_full_diagnostics_with_brief_console_and_one_verdict() -> None:
    logs: list[str] = []
    process, _, _, _ = run_bench_cli_fixture("PASS", "PASS", analysis_logs=logs)
    assert_true(process.returncode == 0, process.stdout + process.stderr)
    assert_true(len(logs) == 1 and logs[0].count("original event frames") == 21, str(logs))
    for detail in ("Reader diagnostic: retained in bench.log", "Standalone analyzer summary: retained in bench.log"):
        assert_true(detail in logs[0] and detail not in process.stdout, process.stdout)
    progress = [line for line in process.stdout.splitlines() if "[bench] display analysis:" in line]
    assert_true(len(progress) == 11, str(progress))
    assert_true("0% (1/10001" in progress[0] and "100% (10001/10001" in progress[-1], str(progress))
    assert_true(process.stdout.count("NO_DIFFERENCES_OBSERVED") == 1, process.stdout)
    assert_true("unresolved comparisons remain unknown" in process.stdout, process.stdout)
    assert_true("encounter-check/report.html" in process.stdout, process.stdout)
    readiness = "[bench] frequency reading: Calibrated frequency fallback unavailable: counter_edges"
    assert_true(process.stdout.count(readiness) == 2, "capture or analysis capability was hidden: " + process.stdout)
    assert_true(process.stdout.index(readiness + " (before collection)") < process.stdout.index("visual behavior: comparing"),
                "capture capability was delayed until analysis")
    assert_true(readiness in logs[0], "analysis capability was not retained in the detailed log")


def test_bench_cli_rejects_old_or_incomplete_product_contract_explicitly() -> None:
    for change in ({"kind": "sampled_encounter_check"}, {"schema_version": 2}):
        payload = generated_encounter_payload(("NO_DIFFERENCES_OBSERVED",))
        payload.update(change)
        process, _, _, _ = run_bench_cli_fixture("PASS", "PASS", encounter_payload=payload)
        assert_true(process.returncode == 2, process.stdout)
        assert_true("unsupported visual behavior summary" in process.stdout, process.stdout)
        assert_true(process.stdout.splitlines()[-1] == "MEASUREMENT_INCOMPLETE (visual behavior)", process.stdout)


def test_bench_cli_recomputes_positive_persistence_sequences_and_preserves_other_gates() -> None:
    from copy import deepcopy
    from bench.encounter_persistence import measure_persistence_behavior, persistence_result
    from encounter_behavior import summarize
    from test_encounter_persistence import CONFIG, K, event, live, span

    def payload_for(outcome):
        payload = generated_encounter_payload(("MEASUREMENT_INCOMPLETE", "NO_DIFFERENCES_OBSERVED", "MEASUREMENT_INCOMPLETE"))
        release = [span(1, "24.150"), span(2, "24.150") if outcome == "retained" else span(2)]
        literal = [event(1, [], [span(1), span(2)]), event(2, [K], [live(1, K), live(2, K)]), event(3, [], release)]
        for original, measured in zip(payload["events"], literal):
            measured["observation"] = {**original["observation"], "input_anchor_ns": 1}
            measured["coverage"] = original["coverage"]
            measured["unresolved_frames"] = original["unresolved_frames"]
            measured["findings"] = []
            measured["phase_observation"] = {"required_phase_ids": ["phase-1"], "observed_phase_ids": ["phase-1"]}
        if outcome == "live_missing":
            literal[1]["observation"]["target_observed"] = False
        if outcome == "coverage_missing":
            literal[1]["coverage"]["unrecorded_source_frames"] = 1
        if outcome == "phase_missing":
            literal[1]["phase_observation"]["required_phase_ids"].append("phase-2")
        payload["events"] = literal
        payload["evidence"]["configuration"] = {"status": "verified", "settings": CONFIG}
        payload["persistence"] = measure_persistence_behavior(literal, CONFIG)
        _, payload["summary"] = summarize(literal, [], payload["reader_qualification"])
        payload["result"] = persistence_result(literal, [], payload["reader_qualification"], payload["persistence"])
        return payload

    for outcome, expected in (("complete", "NO_DIFFERENCES_OBSERVED"), ("retained", "DIFFERENCES_FOUND"),
                              ("live_missing", "MEASUREMENT_INCOMPLETE"), ("coverage_missing", "MEASUREMENT_INCOMPLETE"),
                              ("phase_missing", "MEASUREMENT_INCOMPLETE")):
        payload = payload_for(outcome)
        assert_true(payload["result"] == expected, str(payload))
        process, _, calls, _ = run_bench_cli_fixture("PASS", "PASS", encounter_result=expected,
                                                   encounter_payload=payload, offline=True)
        assert_true(process.returncode == {"NO_DIFFERENCES_OBSERVED": 0, "DIFFERENCES_FOUND": 1, "MEASUREMENT_INCOMPLETE": 2}[expected]
                    and calls == 1 and "analysis result rejected" not in process.stdout,
                    outcome + ": " + process.stdout + process.stderr)
        assert_true("persistence:" in process.stdout and "100 ms" not in process.stdout, process.stdout)
    forged = deepcopy(payload_for("retained"))
    forged["persistence"]["summary"]["findings"] = 0
    process, _, _, _ = run_bench_cli_fixture("PASS", "PASS", encounter_result=forged["result"], encounter_payload=forged, offline=True)
    assert_true(process.returncode == 2 and "persistence result disagrees" in process.stdout, process.stdout + process.stderr)


def test_bench_cli_forwards_build_comparison_only_for_visual_analysis() -> None:
    for offline in (True, False):
        process, _, calls, _ = run_bench_cli_fixture("PASS", "PASS", compare=True, offline=offline)
        assert_true(process.returncode == 0 and calls == 1, process.stdout + process.stderr)
    for options in ({"camera": False}, {"qualification_capture": True}):
        process, _, calls, _ = run_bench_cli_fixture("PASS", "PASS", compare=True, **options)
        assert_true(process.returncode == 2 and calls == 0, process.stdout + process.stderr)


def test_bench_cli_forwards_reading_reuse_only_for_offline_analysis() -> None:
    for compare in (False, True):
        process, counter_calls, calls, _ = run_bench_cli_fixture(
            "PASS", "PASS", offline=True, reuse=True, compare=compare, analysis_ranges=("0:1",))
        assert_true(process.returncode == 0 and counter_calls == 0 and calls == 1, process.stdout + process.stderr)
    for options in ({}, {"camera": False}, {"qualification_capture": True}):
        process, counter_calls, calls, _ = run_bench_cli_fixture("PASS", "PASS", reuse=True, **options)
        assert_true(process.returncode == 2 and counter_calls == calls == 0, process.stdout + process.stderr)


def test_bench_cli_consumes_real_phase_observations_without_an_acquisition_deadline() -> None:
    from copy import deepcopy
    from encounter_behavior import event_findings, summarize
    from encounter_behavior_contract import behavior_contract
    from encounter_observation import summarize_event_observations
    from encounter_phase_observation import measure_event_phases
    from test_encounter_expectation import alert, literals, recording
    from test_encounter_sequence import sequence

    contract = behavior_contract(ROOT, "ee6b401")
    on, off = literals(), literals(active_bands=[], main_arrows=[])
    off["counter_glyph"] = {"state": "absent"}
    inputs = recording([([alert()], [6, 0, 1, 0x24, 0, 12, 12, 0x40])])
    cases = [
        ("phase_not_observed", [deepcopy(on) for _ in range(20)], "MEASUREMENT_INCOMPLETE", 1),
        ("phase_held", [deepcopy(on) for _ in range(121)], "DIFFERENCES_FOUND", 1),
        ("alternating", [deepcopy(on if (i * 5 // 96) % 2 == 0 else off) for i in range(121)], "NO_DIFFERENCES_OBSERVED", 0),
    ]
    for name, values, expected_result, missing in cases:
        seq, *_ = sequence([1.02 + i * .005 for i in range(len(values))], values, inputs=inputs)
        assert_true(not seq["errors"], str(seq["errors"]))
        event = seq["events"][0]
        observation = summarize_event_observations(event)
        observation["first_target_ms"] = (
            observation["first_target_observation"]["capture_ns"] - observation["input_anchor_ns"]) / 1e6
        phases = measure_event_phases(event, contract)
        findings = event_findings(event, observation, contract) + phases["findings"]
        if name == "phase_held":
            assert_true(findings and findings[0]["kind"] == "blink_phase_held", str(findings))
        else:
            assert_true(not findings, str(findings))
        payload = generated_encounter_payload(("NO_DIFFERENCES_OBSERVED",))
        payload["events"] = [{"event_id": event["event_id"], "input_key": [1], "target": event["target"],
                               "observation": observation, "phase_observation": phases,
                               "findings": findings, "coverage": event["coverage"], "unresolved_frames": 0}]
        payload["result"], payload["summary"] = summarize(payload["events"], [], payload["reader_qualification"])
        assert_true(payload["result"] == expected_result and payload["summary"]["events_with_unobserved_blink_phases"] == missing,
                    str(payload["summary"]))
        process, _, calls, _ = run_bench_cli_fixture("PASS", "PASS", encounter_result=payload["result"], encounter_payload=payload)
        assert_true(process.returncode == {"NO_DIFFERENCES_OBSERVED": 0, "DIFFERENCES_FOUND": 1, "MEASUREMENT_INCOMPLETE": 2}[expected_result]
                    and calls == 1 and "analysis result rejected" not in process.stdout, name + ": " + process.stdout)
        if name == "phase_not_observed":
            payload["summary"]["events_with_unobserved_blink_phases"] = 0
            process, _, _, _ = run_bench_cli_fixture("PASS", "PASS", encounter_result=expected_result, encounter_payload=payload)
            assert_true(process.returncode == 2 and "blink phase count disagrees" in process.stdout, process.stdout)


def test_bench_cli_offline_mode_rejects_hardware_flags_and_live_ranges() -> None:
    for arguments in (("--replay",), ("--all",), ("--camera",), ("--no-flash",),
                      ("--qualification-capture",)):
        process, counter_calls, encounter_calls, _ = run_bench_cli_fixture(
            "PASS", "PASS", offline=True, extra_arguments=arguments)
        assert_true(process.returncode == 2 and counter_calls == encounter_calls == 0,
                    process.stdout + process.stderr)
    process, counter_calls, encounter_calls, _ = run_bench_cli_fixture(
        "PASS", "PASS", analysis_ranges=("0:1",))
    assert_true(process.returncode == 2 and counter_calls == encounter_calls == 0,
                process.stdout + process.stderr)


def test_bench_cli_propagates_encounter_verdicts_with_fixed_precedence() -> None:
    for window in ("PASS", "COLLECTION_ONLY"):
        for result, status in (("NO_DIFFERENCES_OBSERVED", 0), ("DIFFERENCES_FOUND", 1), ("MEASUREMENT_INCOMPLETE", 2)):
            process, counter_calls, encounter_calls, _ = run_bench_cli_fixture(window, "PASS", encounter_result=result)
            assert_true(process.returncode == (1 if window == "COLLECTION_ONLY" else status), process.stdout)
            assert_true((counter_calls, encounter_calls) == (1, 1), process.stdout)
            assert_true("[bench] display: target observed" in process.stdout, process.stdout)
            if window == "COLLECTION_ONLY":
                assert_true(f"[bench] visual behavior: {result}" in process.stdout, process.stdout)
            assert_true("2/2 recorded frames read" in process.stdout, process.stdout)
            assert_true("host input acceptance: 10 / 10 packets" in process.stdout, process.stdout)
            assert_true("DUT receipt not observed" in process.stdout, process.stdout)
            assert_true("encounter-check/report.html" in process.stdout, process.stdout)
            if result != "NO_DIFFERENCES_OBSERVED":
                assert_true("encounter-check/report.html#event=event-0001" in process.stdout, process.stdout)
            final = "COLLECTION-ONLY (unqualified:" if window == "COLLECTION_ONLY" else result + " (visual behavior)"
            assert_true(final in process.stdout.splitlines()[-1], process.stdout)
    process, counter_calls, encounter_calls, visual_calls = run_bench_cli_fixture("PASS", "FAIL", visual_exit=2, run_all=True)
    assert_true(process.returncode == 0, process.stdout)
    assert_true((counter_calls, encounter_calls, visual_calls) == (1, 1, 0), process.stdout)
    assert_true("visual timing" not in process.stdout, process.stdout)


def test_bench_cli_preserves_non_camera_and_hard_collection_results() -> None:
    process, counter_calls, encounter_calls, visual_calls = run_bench_cli_fixture("PASS", "PASS", camera=False)
    assert_true(process.returncode == 0, process.stdout)
    assert_true((counter_calls, encounter_calls, visual_calls) == (0, 0, 0), process.stdout)
    assert_true("[bench] sampled live counter: NOT_EVALUATED" in process.stdout, process.stdout)
    assert_true("[bench] visual behavior: NOT_EVALUATED" in process.stdout, process.stdout)
    assert_true(process.stdout.splitlines()[-1] == "PASS", process.stdout)

    process, counter_calls, encounter_calls, visual_calls = run_bench_cli_fixture("FAIL", "PASS")
    assert_true(process.returncode == 2, process.stdout)
    assert_true((counter_calls, encounter_calls, visual_calls) == (0, 0, 0), process.stdout)
    assert_true("[bench] sampled live counter: NOT_EVALUATED" in process.stdout, process.stdout)
    assert_true(process.stdout.splitlines()[-1].startswith("FAIL ("), process.stdout)


def test_bench_cli_qualification_capture_withholds_all_pixel_readers() -> None:
    records: list[dict | None] = []
    process, counter_calls, encounter_calls, visual_calls = run_bench_cli_fixture(
        "PASS", "PASS", qualification_capture=True, capture_records=records)
    assert_true(process.returncode == 0, process.stdout + process.stderr)
    assert_true((counter_calls, encounter_calls, visual_calls) == (0, 0, 0), process.stdout)
    assert_true("camera pixels retained unread; automatic pixel readers disabled" in process.stdout,
                process.stdout)
    assert_true("sampled live counter: NOT_EVALUATED" in process.stdout, process.stdout)
    assert_true("visual behavior: NOT_EVALUATED" in process.stdout, process.stdout)
    assert_true(process.stdout.splitlines()[-1] ==
                "QUALIFICATION-CAPTURED (pixels withheld; visible product NOT_EVALUATED)",
                process.stdout)
    assert_true(len(records) == 1 and isinstance(records[0], dict), str(records))
    record = records[0]
    assert isinstance(record, dict)
    assert_true(record["kind"] == "blind_visible_reader_qualification_capture", str(record))
    assert_true(record["source_git_sha"] == "0123456789abcdef0123456789abcdef01234567",
                str(record))
    assert_true(record["collection"]["result"] == "PASS" and
                record["collection"]["camera_result"] == "CAPTURED", str(record))
    assert_true(record["pixel_analysis"] == {
        "status": "WITHHELD_BY_CAPTURE_MODE",
        "executed": [],
        "disabled": ["counter_check", "encounter_check"],
        "analyzer_outputs_present": False,
    }, str(record))
    assert_true(record["visible_product_eligible"] is False, str(record))

    absent, counter_calls, encounter_calls, visual_calls = run_bench_cli_fixture(
        "PASS", "PASS", qualification_capture=True, camera_present=False)
    assert_true(absent.returncode == 2, absent.stdout + absent.stderr)
    assert_true((counter_calls, encounter_calls, visual_calls) == (0, 0, 0), absent.stdout)
    assert_true(absent.stdout.splitlines()[-1] ==
                "FAIL (qualification capture): requested camera is unavailable", absent.stdout)

    invalid, counter_calls, encounter_calls, visual_calls = run_bench_cli_fixture(
        "PASS", "PASS", camera=False, qualification_capture=True)
    assert_true(invalid.returncode == 2, invalid.stdout + invalid.stderr)
    assert_true((counter_calls, encounter_calls, visual_calls) == (0, 0, 0), invalid.stdout)
    assert_true(invalid.stdout.startswith("FAIL (collection): usage:"), invalid.stdout)


def test_bench_cli_keeps_missing_and_interrupted_encounter_evidence_inconclusive() -> None:
    process, counter_calls, encounter_calls, _ = run_bench_cli_fixture("PASS", "PASS", camera_present=False)
    assert_true(process.returncode == 1 and (counter_calls, encounter_calls) == (0, 0), process.stdout)
    assert_true("visual behavior: MEASUREMENT_INCOMPLETE | requested camera evidence is unavailable" in process.stdout, process.stdout)
    for options in ({"write_encounter_result": False, "encounter_exit": 130}, {"encounter_exit": 2},
                    {"encounter_payload": {"result": "NO_DIFFERENCES_OBSERVED"}}, {"interrupt_encounter": True}):
        process, counter_calls, encounter_calls, _ = run_bench_cli_fixture("PASS", "PASS", **options)
        assert_true(process.returncode == 2, process.stdout + process.stderr)
        assert_true((counter_calls, encounter_calls) == (1, 1), process.stdout)
        assert_true(process.stdout.splitlines()[-1] == "MEASUREMENT_INCOMPLETE (visual behavior)", process.stdout)


def test_bench_cli_preserves_joint_state_failures_and_vetoes_incomplete_pass() -> None:
    payload = generated_encounter_payload(("DIFFERENCES_FOUND", "MEASUREMENT_INCOMPLETE"))
    payload["events"][0]["findings"][0]["field"] = "joint_state"
    process, _, _, _ = run_bench_cli_fixture("PASS", "PASS", encounter_result="DIFFERENCES_FOUND", encounter_payload=payload)
    assert_true(process.returncode == 1, process.stdout)
    assert_true("findings 1 | affected events 1" in process.stdout and "2 frames with unresolved comparisons" in process.stdout, process.stdout)
    for mutation in (
        lambda p: p["summary"].update(findings=0),
        lambda p: p["summary"].update(targets_observed=0),
        lambda p: p["summary"].update(read_frames=100),
        lambda p: p["summary"].update(unresolved_frames=0),
        lambda p: p.update(result="NO_DIFFERENCES_OBSERVED"),
        lambda p: p["events"][0]["observation"]["fields"].pop("main_arrows"),
        lambda p: p["evidence"].update(runtime_identity={}),
        lambda p: p["reader_qualification"].update(status="REJECTED"),
    ):
        from copy import deepcopy
        changed = deepcopy(payload)
        mutation(changed)
        process, _, _, _ = run_bench_cli_fixture("PASS", "PASS", encounter_result="DIFFERENCES_FOUND", encounter_payload=changed)
        assert_true(process.returncode == 2 and "analysis result rejected" in process.stdout, process.stdout)

    # An unresolved transition stays visible without becoming an invented
    # response deadline after the target has independently been observed.
    transient = generated_encounter_payload(("TRANSIENT_UNKNOWN",))
    process, _, _, _ = run_bench_cli_fixture("PASS", "PASS", encounter_payload=transient)
    assert_true(process.returncode == 0 and "1 frames with unresolved comparisons" in process.stdout, process.stdout)

    from encounter_behavior import summarize
    dropped = generated_encounter_payload(("NO_DIFFERENCES_OBSERVED",))
    dropped["events"][0]["coverage"]["unrecorded_source_frames"] = 1
    dropped["result"], dropped["summary"] = summarize(dropped["events"], [], dropped["reader_qualification"])
    process, _, _, _ = run_bench_cli_fixture("PASS", "PASS", encounter_result=dropped["result"], encounter_payload=dropped)
    assert_true(process.returncode == 2 and "analysis result rejected" not in process.stdout, process.stdout)

    incomplete = generated_encounter_payload(("DIFFERENCES_FOUND",))
    incomplete["errors"] = ["Incomplete capture evidence"]
    incomplete["result"] = "MEASUREMENT_INCOMPLETE"
    process, _, _, _ = run_bench_cli_fixture("PASS", "PASS", encounter_result="MEASUREMENT_INCOMPLETE", encounter_payload=incomplete)
    assert_true(process.returncode == 2 and "findings 1 | affected events 1" in process.stdout, process.stdout)



class FakeClock:
    def __init__(self) -> None:
        self.now = 0.0

    def monotonic(self) -> float:
        return self.now


class FakeTimeline:
    def __init__(self) -> None:
        self.events: list[tuple[str, dict[str, Any]]] = []

    def record(self, event: str, **fields: Any) -> None:
        self.events.append((event, fields))


class FakeSerialObserver:
    def __init__(self, clock: FakeClock, lines: dict[int, str]) -> None:
        self.clock = clock
        self.lines = lines
        self.read_count = 0
        self.identity_tracker = RuntimeIdentityTracker()
        self.timeline = FakeTimeline()
        self.reset_performed = False

    @property
    def boot_marker_count(self) -> int:
        return self.identity_tracker.boot_marker_count

    @property
    def runtime_identity(self) -> dict[str, Any] | None:
        return self.identity_tracker.identity

    def read_line(self, timeout_s: float) -> str:
        self.clock.now += timeout_s
        self.read_count += 1
        line = self.lines.get(self.read_count, "")
        self.identity_tracker.observe(line)
        return line


def test_serial_boundary_waits_for_attach_time_boot_past_initial_observation() -> None:
    clock = FakeClock()
    observer = FakeSerialObserver(
        clock,
        {
            8: "ESP-ROM:esp32s3-20210327",
            10: "BOOT bootId=4 uptimeMs=2053 reset=USB git=2f32dda image=04904e028",
        },
    )
    result = establish_serial_boundary(
        observer, 5.0, monotonic=clock.monotonic  # type: ignore[arg-type]
    )
    assert_true(result["mode"] == "startup_completed", str(result))
    assert_true(result["startup_detected"] is True, str(result))
    assert_true(result["boot_markers_observed"] == 1, str(result))
    assert_true(clock.now > 2.0, f"boundary did not extend past warmup: {clock.now}")
    assert_true(
        observer.timeline.events[-1][0] == "serial_boundary_established",
        str(observer.timeline.events),
    )


def test_missing_and_malformed_boot_identity_fail() -> None:
    clock = FakeClock()
    observer = FakeSerialObserver(clock, {})
    assert_identity_failure(
        lambda: establish_serial_boundary(
            observer, 2.0, monotonic=clock.monotonic  # type: ignore[arg-type]
        ),
        "runtime BOOT identity was not observed",
    )
    assert_identity_failure(
        lambda: parse_runtime_boot_identity(
            "BOOT bootId=4 uptimeMs=2053 reset=USB git=2f32dda image=bad"
        ),
        "malformed runtime BOOT identity",
    )


def test_conflicting_boot_identities_fail() -> None:
    tracker = RuntimeIdentityTracker()
    tracker.observe(
        "BOOT bootId=4 uptimeMs=2053 reset=USB git=2f32dda image=04904e028"
    )
    assert_identity_failure(
        lambda: tracker.observe(
            "BOOT bootId=5 uptimeMs=2040 reset=SW git=2f32dda image=111111111"
        ),
        "runtime BOOT identity changed",
    )


def test_serial_boundary_fails_if_detected_startup_never_reaches_boot_identity() -> None:
    clock = FakeClock()
    observer = FakeSerialObserver(clock, {1: "rst:0x15 (USB_UART_CHIP_RESET)"})
    try:
        establish_serial_boundary(
            observer, 3.0, monotonic=clock.monotonic  # type: ignore[arg-type]
        )
    except RuntimeError as exc:
        assert_true("runtime BOOT identity was not observed" in str(exc), str(exc))
    else:
        raise AssertionError("incomplete startup was admitted to the evidence window")


def test_native_usb_reset_refuses_other_ports_before_control_changes() -> None:
    calls = []
    ports = SimpleNamespace(comports=lambda: [SimpleNamespace(device="/fixture", vid=0x303A, pid=0x1001)])
    with mock.patch.dict(sys.modules, {
        "serial.tools": SimpleNamespace(list_ports=ports),
    }), mock.patch.object(run_window_module.time, "sleep", lambda seconds: calls.append(("wait", seconds))):
        port = SimpleNamespace(port="/fixture", dtr=False,
                               setRTS=lambda value: calls.append(("RTS", value)),
                               setDTR=lambda value: calls.append(("DTR", value)))
        reset, metadata = run_window_module.native_usb_reset_strategy(port)
        assert_true(not calls, "loading reset strategy changed device state")
        reset()
        assert_true(calls == [("RTS", True), ("DTR", False), ("wait", 0.1),
                              ("RTS", False), ("DTR", False)], str(calls))
        assert_true(metadata["usb_vid"] == 0x303A and metadata["usb_pid"] == 0x1001, str(metadata))
        ports.comports = lambda: [SimpleNamespace(device="/fixture", vid=0x1234, pid=0x1001)]
        try:
            run_window_module.native_usb_reset_strategy(port)
        except RuntimeError as exc:
            assert_true("native USB Serial/JTAG" in str(exc), str(exc))
        else:
            raise AssertionError("reset admitted an unrelated USB device")
        assert_true(len(calls) == 5, "unrelated USB device reached reset strategy")


def test_explicit_reset_records_request_before_control_and_never_completes_failure() -> None:
    for fail in (False, True):
        events = []
        observer = run_window_module.BenchSerial.__new__(run_window_module.BenchSerial)
        observer.ser = SimpleNamespace(reset_input_buffer=lambda: events.append("drain"))
        observer.identity_tracker = RuntimeIdentityTracker()
        observer.identity_tracker.identity = dict(RUNTIME_IDENTITY)
        observer._pending_lines = ["BOOT stale pre-reset fragment"]

        def record(event, **_fields):
            events.append(event)
            return {"host_monotonic_ns": 1_000_000_000}

        observer.timeline = SimpleNamespace(record=record)

        def reset() -> None:
            events.append("control_reset")
            if fail:
                raise OSError("unsupported RTS")

        try:
            observer.reset_for_boot(reset_factory=lambda _: (reset, {"strategy": "fixture"}))
        except OSError:
            assert_true(fail, "successful reset raised")
        expected = ["drain", "serial_reset_requested", "control_reset"]
        if not fail:
            expected.append("serial_reset_completed")
        assert_true(events == expected, str(events))
        assert_true(observer.reset_performed is not fail, "reset failure acquired a completed anchor")
        assert_true(observer.runtime_identity is None, "pre-reset identity survived")
        assert_true(observer._pending_lines == [], "pre-reset line fragment survived")


def test_explicit_boundary_requires_fresh_usb_reset_and_refuses_intervening_failure() -> None:
    rom = "ESP-ROM:esp32s3-20210327"
    reason = "rst:0x15 (USB_UART_CHIP_RESET),boot:0xa (SPI_FAST_FLASH_BOOT)"
    boot = "BOOT bootId=4 uptimeMs=2053 reset=USB git=2f32dda image=04904e028"
    ready = "[Boot] Ready gate opened at 2380 ms"
    complete = "[Boot] setup total: 2262 ms"
    cases = [
        ([rom, reason, boot, ready, complete], None),
        ([reason, boot, ready, complete], None),
        ([reason, "Saved PC:0x", rom, "Build:Mar 27 2021", reason, boot, ready, complete], None),
        ([boot], "preceded fresh reset-to-ready"),
        ([rom, boot], "preceded fresh reset-to-ready"),
        ([rom, "rst:0xc (RTC_SW_CPU_RST),boot:0xa (SPI_FAST_FLASH_BOOT)", boot], "unexpected reset reason"),
        ([rom, reason, rom, reason, boot], "repeated ROM start"),
        ([rom, reason, "Guru Meditation Error: panic", boot], "panic or brownout"),
        ([rom, reason, boot.replace("reset=USB", "reset=PANIC")], "reset reason does not match"),
        ([rom, reason, boot.replace("reset=USB", "reset=SW")], "reset reason does not match"),
        ([rom, reason, boot, ready], "was not observed"),
        ([rom, reason, boot, complete], "was not observed"),
        ([rom, reason, boot, "Guru Meditation Error: panic", ready, complete], "panic or brownout"),
        ([rom, reason, boot, boot, ready, complete], "repeated runtime BOOT"),
    ]
    for lines, error in cases:
        clock = FakeClock()
        observer = FakeSerialObserver(clock, dict(enumerate(lines, start=1)))
        observer.reset_performed = True
        action = lambda: establish_serial_boundary(observer, 5, monotonic=clock.monotonic,
                                                    require_explicit_reset=True)
        if error:
            assert_identity_failure(action, error)
            assert_true(not observer.timeline.events, "failed startup published a verified boundary")
        else:
            result = action()
            assert_true(result["reset_anchored"] is True, str(result))
            assert_true(result["runtime_identity"]["boot_id"] == 4, str(result))
            assert_true(observer.read_count == 5, "boundary returned before normal setup completed")
    observer = FakeSerialObserver(FakeClock(), {})
    assert_identity_failure(lambda: establish_serial_boundary(observer, 5, require_explicit_reset=True),
                            "explicit serial reset did not complete")


def test_serial_carriage_return_framing_preserves_reset_evidence_and_failures() -> None:
    rom = "ESP-ROM:esp32s3-20210327"
    suffix = ["rst:0x15 (USB_UART_CHIP_RESET),boot:0xa (SPI_FAST_FLASH_BOOT)",
              "BOOT bootId=4 uptimeMs=2053 reset=USB git=2f32dda image=04904e028",
              "[Boot] Ready gate opened at 2380 ms", "[Boot] setup total: 2262 ms"]
    for prefix, trailing, error in (("load:0x3fce2820,len:0x10cc", "", None),
                          ("Guru Meditation Error: panic", "", "panic or brownout"),
                          (rom, "", "repeated ROM start"),
                          ("load:0x3fce2820,len:0x10cc", rom, "repeated ROM start"),
                          ("load:0x3fce2820,len:0x10cc", "Guru Meditation Error: panic", "panic or brownout")):
        ending = suffix[:-1] + [suffix[-1] + ("\r" + trailing if trailing else "")]
        chunks = iter([(prefix + "\r" + rom + "\n").encode(),
                       *[(line + "\r\n").encode() for line in ending]])
        observer = run_window_module.BenchSerial.__new__(run_window_module.BenchSerial)
        clock = FakeClock()
        received = []

        def read() -> bytes:
            clock.now += 0.01
            return next(chunks, b"")

        def record(event, **fields):
            received.append((event, fields))
            return {"host_monotonic_ns": int(clock.now * 1e9)}

        observer.ser = SimpleNamespace(timeout=0.25, readline=read)
        observer.log = io.StringIO()
        observer.timeline = SimpleNamespace(record=record)
        observer.identity_tracker = RuntimeIdentityTracker()
        observer.line_count = 0
        observer._pending_lines = []
        observer.reset_performed = True
        action = lambda: establish_serial_boundary(observer, 5, monotonic=clock.monotonic,
                                                    require_explicit_reset=True)
        if error:
            assert_identity_failure(action, error)
            assert_true(not any(event == "serial_boundary_established" for event, _ in received),
                        "framing hid a failed reset boundary")
        else:
            result = action()
            assert_true(result["reset_anchored"] and observer.boot_marker_count == 1, str(result))
            lines = [fields["line"] for event, fields in received if event == "serial_receive"]
            assert_true(lines == [prefix, rom, *suffix], str(lines))
            assert_true(observer.log.getvalue().splitlines() == lines, "serial log dropped a fragment")


def test_serial_interrupted_loader_framing_requires_one_exact_rom_banner() -> None:
    rom = "ESP-ROM:esp32s3-20210327"
    reason = "rst:0x15 (USB_UART_CHIP_RESET),boot:0xa (SPI_FAST_FLASH_BOOT)"
    suffix = ["Build:Mar 27 2021", reason,
              "BOOT bootId=4 uptimeMs=2053 reset=USB git=2f32dda image=04904e028",
              "[Boot] Ready gate opened at 2380 ms", "[Boot] setup total: 2262 ms"]
    # Every nonempty byte position is a possible interruption, including within
    # "load:0x", the comma and "len:0x". Do not special-case observed fragments.
    loader_lines = ("load:0x3fce2820,len:0x10cc", "load:0xA,len:0xB")
    prefixes = sorted({line[:stop] for line in loader_lines for stop in range(1, len(line) + 1)})
    cases = [(prefix + rom, suffix, [prefix, rom], None) for prefix in prefixes]
    reset_prefixes = sorted({reason[:stop] for stop in range(1, len(reason))})
    cases += [(prefix + rom, suffix, [prefix, rom], None) for prefix in reset_prefixes]
    saved_pc_lines = ("Saved PC:0x40380000", "Saved PC:0xA")
    saved_pc_prefixes = sorted({line[:stop] for line in saved_pc_lines for stop in range(1, len(line) + 1)})
    cases += [(prefix + rom, suffix, [prefix, rom], None) for prefix in saved_pc_prefixes]
    cases += [(prefix + rom + rom, suffix, None, "unexpected reset reason")
              for prefix in prefixes]
    cases += [(prefix + "Guru Meditation Error: panic" + rom, suffix,
               None, "panic or brownout") for prefix in prefixes]
    invalid_prefixes = ("", "notice ", "load:0xnothex", "load:0x,len:0x1",
                        "load:0x1;len:0x2", "load:0x1,len:1", "load:0x1,len:0xG",
                        "load:0x1,len:0x2,", "load:0x1 len:0x2", "load:0x1,len:0x2 ")
    for prefix in invalid_prefixes:
        assert_true(not run_window_module._is_rom_loader_prefix(prefix),
                    f"invalid ROM loader prefix accepted: {prefix!r}")
        assert_true(not run_window_module._is_interrupted_usb_reset_prefix(prefix),
                    f"invalid USB reset prefix accepted: {prefix!r}")
        assert_true(not run_window_module._is_rom_saved_pc_prefix(prefix),
                    f"invalid saved-PC prefix accepted: {prefix!r}")
    cases += [(prefix + rom, suffix, None, "unexpected reset reason")
              for prefix in invalid_prefixes if prefix]
    cases += [
        ("load:0x3fce2" + rom + rom, suffix, None, "unexpected reset reason"),
        ("load:0x3fce2" + rom + "Guru Meditation Error: panic", suffix,
         None, "panic or brownout"),
        ("Guru Meditation Error: panic" + rom, suffix, None, "panic or brownout"),
        (rom + rom, suffix, None, "unexpected or repeated ROM start"),
        (rom + "garbage", suffix, None, "unexpected or repeated ROM start"),
        ("ESP-ROM:esp32-20210327", suffix, None, "unexpected or repeated ROM start"),
        ("load:0x3fce2" + rom,
         [suffix[0], "rst:0xc (RTC_SW_CPU_RST),boot:0xa (SPI_FAST_FLASH_BOOT)", *suffix[2:]],
         None, "unexpected reset reason"),
        ("load:0x3fce2" + rom, [suffix[0], reason, rom, *suffix[2:]],
         None, "unexpected or repeated ROM start"),
    ]
    for first, ending, expected_prefix, error in cases:
        chunks = iter([(first + "\r\n").encode(),
                       *[(line + "\r\n").encode() for line in ending]])
        observer = run_window_module.BenchSerial.__new__(run_window_module.BenchSerial)
        clock = FakeClock()
        received = []

        def read() -> bytes:
            clock.now += 0.01
            return next(chunks, b"")

        def record(event, **fields):
            received.append((event, fields))
            return {"host_monotonic_ns": int(clock.now * 1e9)}

        observer.ser = SimpleNamespace(timeout=0.25, readline=read)
        observer.log = io.StringIO()
        observer.timeline = SimpleNamespace(record=record)
        observer.identity_tracker = RuntimeIdentityTracker()
        observer.line_count = 0
        observer._pending_lines = []
        observer.reset_performed = True
        action = lambda: establish_serial_boundary(observer, 5, monotonic=clock.monotonic,
                                                    require_explicit_reset=True)
        if error:
            assert_identity_failure(action, error)
            assert_true(not any(event == "serial_boundary_established" for event, _ in received),
                        "interrupted-loader framing hid a failed reset boundary")
        else:
            result = action()
            assert_true(result["reset_anchored"] and observer.boot_marker_count == 1, str(result))
            lines = [fields["line"] for event, fields in received if event == "serial_receive"]
            assert_true(lines == [*expected_prefix, *ending], str(lines))
            assert_true(observer.log.getvalue().splitlines() == lines,
                        "interrupted loader or fresh ROM evidence was discarded")


def test_managed_runner_shows_readiness_before_child_finishes() -> None:
    """Exercise the real quiet wrapper: early capability is visible; diagnostics stay logged."""
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        release = root / "release"
        child = (
            "import pathlib,sys,time\n"
            "print('ordinary detailed output', flush=True)\n"
            "print('detailed stderr', file=sys.stderr, flush=True)\n"
            "print('[bench] frequency reading: Calibrated frequency fallback UNAVAILABLE', flush=True)\n"
            "deadline=time.monotonic()+10\n"
            "while not pathlib.Path(sys.argv[1]).exists() and time.monotonic()<deadline: time.sleep(.01)\n"
        )
        logs = [root / name for name in ("stdout.log", "stderr.log", "combined.log")]
        process = subprocess.Popen([
            sys.executable, str(ROOT / "scripts/bench/run_logged.py"),
            "--stdout", str(logs[0]), "--stderr", str(logs[1]), "--combined", str(logs[2]),
            "--quiet", "--terminal-prefix", "[bench] frequency reading:",
            "--", sys.executable, "-c", child, str(release),
        ], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            ready, _, _ = select.select([process.stdout], [], [], 5)
            assert_true(bool(ready), "readiness remained hidden while collection was running")
            line = process.stdout.readline()
            assert_true(line.startswith("[bench] frequency reading:") and "UNAVAILABLE" in line, line)
            assert_true(process.poll() is None, "readiness was delayed until the child completed")
        finally:
            release.touch()
            remaining, errors = process.communicate(timeout=15)
        assert_true(process.returncode == 0 and not remaining and not errors, "quiet wrapper exposed unrelated output")
        assert_true("ordinary detailed output" in logs[0].read_text(), "stdout log lost full detail")
        assert_true("detailed stderr" in logs[1].read_text(), "stderr log lost full detail")
        assert_true("UNAVAILABLE" in logs[2].read_text(), "combined log lost early capability")


def main() -> int:
    test_managed_runner_shows_readiness_before_child_finishes()
    test_file_artifact_owns_raw_bytes()
    test_build_artifacts_retain_exact_application_after_build_cache_changes()
    test_build_application_retention_refuses_missing_empty_changed_or_failed_copy()
    test_live_collection_refuses_a_retained_application_before_starting()
    test_reused_live_output_refusal_preserves_existing_evidence()
    test_replay_stimulus_is_persisted_as_raw_ndjson_once()
    test_replay_delivery_is_persisted_with_explicit_loss_denominators()
    test_delivery_summary_counts_attempts_and_vetoes_dropped_or_skipped()
    test_delivery_summary_vetoes_malformed_sequence_lifecycles()
    test_delivery_summary_vetoes_empty_instrumentation_stream()
    test_runner_logs_are_confined_to_the_run_directory()
    test_timeline_keeps_ordered_external_events_and_scrubs_private_paths()
    test_post_window_configuration_requires_conservative_same_boot_emission()
    test_post_window_configuration_fails_closed()
    test_post_window_configuration_timeout_covers_clock_allowance()
    test_replay_process_requests_raw_machine_and_scenario_evidence()
    test_requested_dropped_complete_stopped_is_not_completed()
    test_emulator_cleanup_before_start_preserves_primary_failure()
    test_requested_accepted_complete_stopped_is_completed()
    test_radio_lease_excludes_concurrent_owners_and_rejects_symlink_parent()
    test_runner_source_is_external_only_and_serial_is_read_only()
    test_upload_exact_match_is_qualified()
    test_upload_git_mismatch_fails()
    test_upload_image_mismatch_fails()
    test_no_flash_git_match_with_linked_resident_artifact_is_qualified()
    test_no_flash_git_match_with_unlinked_resident_artifact_is_collection_only()
    test_no_flash_git_mismatch_fails()
    test_resident_upload_reference_binds_original_bytes_and_fresh_identity()
    test_resident_upload_reference_rejects_unbound_evidence()
    test_resident_original_no_flash_reference_preserves_identity_without_upload_claim()
    test_resident_no_flash_reference_rejects_mode_drift_chaining_and_unbound_evidence()
    test_resident_reference_cli_requires_pair_without_upload()
    test_resident_collection_keeps_tooling_identity_separate_and_never_uploads()
    test_main_writes_collection_only_and_returns_exit_one_for_unlinked_no_flash()
    test_dirty_source_vetoes_qualification_before_collection()
    test_top_level_pass_is_vetoed_by_delivery_loss_counters()
    test_top_level_pass_is_vetoed_by_malformed_delivery_sequence()
    test_top_level_pass_is_vetoed_by_empty_delivery_stream()
    test_clean_source_preserves_qualified_pass_behavior()
    test_bench_cli_collection_only_branch_has_no_pass_verdict()
    test_bench_cli_uses_selected_reader_environment_and_rejects_before_work()
    test_bench_cli_consumes_current_producer_results_online_and_offline()
    test_bench_cli_keeps_full_diagnostics_with_brief_console_and_one_verdict()
    test_bench_cli_rejects_old_or_incomplete_product_contract_explicitly()
    test_bench_cli_recomputes_positive_persistence_sequences_and_preserves_other_gates()
    test_bench_cli_forwards_build_comparison_only_for_visual_analysis()
    test_bench_cli_forwards_reading_reuse_only_for_offline_analysis()
    test_bench_cli_consumes_real_phase_observations_without_an_acquisition_deadline()
    test_bench_cli_offline_mode_rejects_hardware_flags_and_live_ranges()
    test_bench_cli_propagates_encounter_verdicts_with_fixed_precedence()
    test_bench_cli_preserves_non_camera_and_hard_collection_results()
    test_bench_cli_qualification_capture_withholds_all_pixel_readers()
    test_bench_cli_keeps_missing_and_interrupted_encounter_evidence_inconclusive()
    test_bench_cli_preserves_joint_state_failures_and_vetoes_incomplete_pass()
    test_serial_boundary_waits_for_attach_time_boot_past_initial_observation()
    test_missing_and_malformed_boot_identity_fail()
    test_conflicting_boot_identities_fail()
    test_serial_boundary_fails_if_detected_startup_never_reaches_boot_identity()
    test_native_usb_reset_refuses_other_ports_before_control_changes()
    test_explicit_reset_records_request_before_control_and_never_completes_failure()
    test_explicit_boundary_requires_fresh_usb_reset_and_refuses_intervening_failure()
    test_serial_carriage_return_framing_preserves_reset_evidence_and_failures()
    test_serial_interrupted_loader_framing_requires_one_exact_rom_banner()
    print("bench window tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
