#!/usr/bin/env python3
"""Focused regressions for raw bench collection and direct leg checks."""

from __future__ import annotations

import fcntl
import hashlib
import json
import os
import stat
import subprocess
import sys
import tempfile
from pathlib import Path
from types import SimpleNamespace
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts" / "bench"))

import run_window as run_window_module  # noqa: E402
from run_window import (  # noqa: E402
    BENCH_TIMELINE_NAME,
    REPLAY_STIMULUS_NAME,
    BenchTimeline,
    ReconnectBehaviorError,
    V1Emulator,
    V1RadioLease,
    display_commit_csv_sd_path,
    encounter_csv_sd_path,
    file_artifact,
    panic_sidecar_path,
    publish_replay_stimulus_evidence,
    resolve_runner_log_paths,
    run_reconnect_preflight,
    validate_metrics,
)


def assert_true(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def test_raw_sd_paths_preserve_one_boot_identity() -> None:
    perf = "/perf/perf_boot_42-abc.csv"
    assert_true(
        encounter_csv_sd_path(perf) == "/encounters/encounters_42-abc.csv",
        "encounter path lost the perf boot identity",
    )
    assert_true(
        display_commit_csv_sd_path(perf)
        == "/display_commits/display_commits_42-abc.csv",
        "display commit path lost the perf boot identity",
    )
    assert_true(
        panic_sidecar_path(perf) == "/perf/perf_boot_42-abc.panic.jsonl",
        "panic sidecar was not adjacent to the perf CSV",
    )
    for unsafe in ("", "/tmp/perf.csv", "/perf/perf_boot_.csv", "/perf/perf_boot_../x.csv"):
        assert_true(encounter_csv_sd_path(unsafe) == "", f"unsafe path was accepted: {unsafe}")
        assert_true(display_commit_csv_sd_path(unsafe) == "", f"unsafe path was accepted: {unsafe}")


def test_file_artifact_owns_raw_bytes() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "raw.ndjson"
        payload = b'{"sample":1}\n'
        path.write_bytes(payload)
        artifact = file_artifact(path)
        assert_true(artifact["status"] == "captured", str(artifact))
        assert_true(artifact["path"] == path.name, str(artifact))
        assert_true(artifact["size_bytes"] == len(payload), str(artifact))
        assert_true(artifact["sha256"] == hashlib.sha256(payload).hexdigest(), str(artifact))
        assert_true(
            file_artifact(None, "not_applicable")
            == {"status": "unavailable", "path": "", "reason": "not_applicable"},
            "unavailable raw artifact shape changed",
        )


def test_replay_stimulus_is_persisted_as_raw_ndjson_once() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        out_dir = Path(tmp)
        events = [
            {"state": "stimulus_requested", "sample": 1, "hostMonotonicSeconds": 1.25},
            {"state": "stimulus_requested", "sample": 2, "hostMonotonicSeconds": 1.50},
        ]
        emulator = {"stimulus_events": list(events)}
        result = publish_replay_stimulus_evidence(
            emulator,
            out_dir,
            suite="replay",
            live=True,
        )
        assert_true(result is not None and result["status"] == "captured", str(result))
        assert_true("stimulus_events" not in emulator, "raw events remained duplicated in metadata")
        path = out_dir / REPLAY_STIMULUS_NAME
        decoded = [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()]
        assert_true(decoded == events, f"raw stimulus events changed: {decoded}")
        assert_true(result["sha256"] == hashlib.sha256(path.read_bytes()).hexdigest(), str(result))

        duplicate = publish_replay_stimulus_evidence(
            {"stimulus_events": list(events)},
            out_dir,
            suite="replay",
            live=True,
        )
        assert_true(duplicate is not None and duplicate["status"] == "unavailable", str(duplicate))
        assert_true(decoded == [json.loads(line) for line in path.read_text().splitlines()], "raw file changed")


def test_runner_logs_are_confined_to_the_run_directory() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        out_dir = Path(tmp)
        args = SimpleNamespace(
            runner_stdout_log=str(out_dir / "run.log"),
            runner_stderr_log=str(out_dir / "run.err"),
        )
        paths = resolve_runner_log_paths(args, out_dir)
        assert_true(
            paths
            == {
                "stdout": (out_dir / "run.log").resolve(),
                "stderr": (out_dir / "run.err").resolve(),
            },
            str(paths),
        )
        args.runner_stdout_log = str(out_dir / "elsewhere.log")
        try:
            resolve_runner_log_paths(args, out_dir)
        except ValueError:
            pass
        else:
            raise AssertionError("runner stdout escaped its exact owned path")


def test_timeline_keeps_ordered_raw_events_and_scrubs_private_paths() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / BENCH_TIMELINE_NAME
        timeline = BenchTimeline(path)
        timeline.record("serial_line", value="/Users/private/operator")
        timeline.record_external(
            {"state": "handshake_ready", "active": True},
            "v1replay",
        )
        timeline.close()
        text = path.read_text(encoding="utf-8")
        assert_true("/Users/private/operator" not in text, "timeline leaked a private path")
        records = [json.loads(line) for line in text.splitlines()]
        assert_true(records[0]["event"] == "timeline_opened", str(records))
        assert_true(records[-1]["event"] == "timeline_closed", str(records))
        assert_true(
            any(record.get("state") == "handshake_ready" for record in records),
            "child machine event was not preserved",
        )


def test_metric_validator_receives_only_raw_validation_inputs() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        out_dir = Path(tmp)
        csv_path = out_dir / "perf.csv"
        csv_path.write_text("millis,value\n0,1\n", encoding="utf-8")
        captured: list[str] = []
        original = run_window_module.subprocess.run

        def fake_run(command: list[str], **_kwargs: Any) -> subprocess.CompletedProcess[str]:
            captured.extend(command)
            return subprocess.CompletedProcess(command, 0, "PASS raw metrics valid\n", "")

        run_window_module.subprocess.run = fake_run  # type: ignore[assignment]
        try:
            process = validate_metrics(
                SimpleNamespace(suite="display", segment="last"),
                csv_path,
                out_dir,
            )
        finally:
            run_window_module.subprocess.run = original
        assert_true(process.returncode == 0, str(process))
        assert_true(
            captured
            == [
                sys.executable,
                str(ROOT / "tools" / "import_perf_csv.py"),
                "--input",
                str(csv_path),
                "--suite",
                "display",
                "--segment",
                "last",
            ],
            f"validator received transformed-output arguments: {captured}",
        )


def _write_executable(path: Path) -> None:
    path.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
    path.chmod(path.stat().st_mode | stat.S_IXUSR)


def test_replay_process_uses_raw_machine_events_and_notification_hold() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        executable = root / "build" / "v1replay"
        executable.parent.mkdir(parents=True)
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
            handshake_only=True,
            handshake_notification_hold_ms=125,
            scenario="fixture.json",
        )
        try:
            emulator.start()
        finally:
            emulator._close_log()
            run_window_module.subprocess.Popen = original

        assert_true("--machine-events" in captured, f"machine events not requested: {captured}")
        assert_true("--handshake-only" in captured, f"handshake-only mode not requested: {captured}")
        hold = captured.index("--handshake-notification-hold-ms")
        assert_true(captured[hold + 1] == "125", f"notification hold changed: {captured}")
        assert_true("--scenario-evidence" in captured, f"raw scenario evidence missing: {captured}")


def test_reconnect_readiness_requires_ready_and_active_transport_events() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        executable = Path(tmp) / "v1replay"
        _write_executable(executable)
        emulator = V1Emulator(executable, Path(tmp), "replay", handshake_only=True)
        emulator.health_problem = lambda: ""  # type: ignore[method-assign]
        emulator._bench_event = lambda state: (  # type: ignore[method-assign]
            {"state": "handshake_ready"}
            if state == "handshake_ready"
            else {"state": "handshake_transport", "active": True}
        )
        emulator.wait_for_handshake_ready(0.05)

        emulator._bench_event = lambda state: (  # type: ignore[method-assign]
            {"state": "handshake_ready"}
            if state == "handshake_ready"
            else {"state": "handshake_transport", "active": False}
        )
        try:
            emulator.wait_for_handshake_ready(0.01)
        except ReconnectBehaviorError as exc:
            assert_true(exc.kind == "handshake_timeout", str(exc))
        else:
            raise AssertionError("inactive handshake transport was accepted")


def test_managed_shutdown_checks_complete_machine_event_order() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        executable = Path(tmp) / "v1replay"
        _write_executable(executable)
        emulator = V1Emulator(executable, Path(tmp), "core")
        events = [
            {"state": "session_transport", "active": True},
            {"state": "stopping", "sessionTransportActive": True},
            {"state": "session_transport", "active": False},
            {"state": "stopped"},
        ]
        emulator.returncode = 0
        emulator._ordered_machine_events = lambda: list(events)  # type: ignore[method-assign]
        evidence = emulator._validate_managed_shutdown()
        assert_true(evidence[1:] == (1, 3), str(evidence))
        emulator.session_transport_owned = True
        emulator._managed_shutdown_evidence = evidence
        emulator._validate_idle_shutdown()
        assert_true(emulator.session_transport_continuous, "continuous ownership was not admitted")

        broken = list(events)
        broken.insert(2, {"state": "session_transport", "active": True})
        emulator._ordered_machine_events = lambda: broken  # type: ignore[method-assign]
        try:
            emulator._validate_managed_shutdown()
        except RuntimeError:
            pass
        else:
            raise AssertionError("transport reactivation during teardown was accepted")


def test_reconnect_preflight_checks_cleanup_on_one_serial_session() -> None:
    class FakeSerial:
        boot_marker_count = 0
        disconnect_cleanup_count = 0

        def __init__(self) -> None:
            self.boundaries: list[str] = []

        def record_host_boundary(self, name: str) -> None:
            self.boundaries.append(name)

        def read_line(self, _timeout: float) -> str:
            self.disconnect_cleanup_count += 1
            return run_window_module.V1_DISCONNECT_CLEANUP_PREFIX + "fixture"

    class FakeEmulator:
        def start(self) -> None:
            pass

        def wait_for_handshake_ready(self, _timeout: float) -> None:
            pass

        def health_problem(self) -> str:
            return ""

        def _bench_event(self, state: str) -> dict[str, Any]:
            return {"state": state, "active": True}

        def finish_preflight(self, handshake_ready_while_alive: bool) -> dict[str, Any]:
            return {
                "handshake_ready_while_alive": handshake_ready_while_alive,
                "confirmed_exit": True,
            }

    original = run_window_module.establish_serial_fence
    run_window_module.establish_serial_fence = lambda *_args, **_kwargs: {}  # type: ignore[assignment]
    serial = FakeSerial()
    try:
        result = run_reconnect_preflight(serial, FakeEmulator(), 0.1)
    finally:
        run_window_module.establish_serial_fence = original
    assert_true(result["cleanup_marker_count"] == 1, str(result))
    assert_true(result["serial_session_continuous"] is True, str(result))
    assert_true(
        serial.boundaries[0] == run_window_module.RECONNECT_PREFLIGHT_START,
        f"preflight did not begin at an owned host boundary: {serial.boundaries}",
    )


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
            flags = fcntl.fcntl(owner.fd, fcntl.F_GETFD)
            assert_true(flags & fcntl.FD_CLOEXEC == 0, "lease descriptor cannot reach the child")

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


def test_runner_source_uses_raw_event_contract() -> None:
    source = (ROOT / "scripts" / "bench" / "run_window.py").read_text(encoding="utf-8")
    assert_true('self._bench_event("handshake_ready")' in source, "raw ready event is not checked")
    assert_true('self._bench_event("handshake_transport")' in source, "raw transport event is not checked")
    assert_true("validate_metrics(args, csv_path, out_dir)" in source, "raw metrics are not validated")


def main() -> int:
    test_raw_sd_paths_preserve_one_boot_identity()
    test_file_artifact_owns_raw_bytes()
    test_replay_stimulus_is_persisted_as_raw_ndjson_once()
    test_runner_logs_are_confined_to_the_run_directory()
    test_timeline_keeps_ordered_raw_events_and_scrubs_private_paths()
    test_metric_validator_receives_only_raw_validation_inputs()
    test_replay_process_uses_raw_machine_events_and_notification_hold()
    test_reconnect_readiness_requires_ready_and_active_transport_events()
    test_managed_shutdown_checks_complete_machine_event_order()
    test_reconnect_preflight_checks_cleanup_on_one_serial_session()
    test_radio_lease_excludes_concurrent_owners_and_rejects_symlink_parent()
    test_runner_source_uses_raw_event_contract()
    print("bench window tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
