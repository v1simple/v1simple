#!/usr/bin/env python3
"""Focused regressions for the external-only bench evidence contract."""

from __future__ import annotations

import fcntl
import hashlib
import json
import os
import stat
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
    V1Emulator,
    V1RadioLease,
    file_artifact,
    publish_replay_stimulus_evidence,
    resolve_runner_log_paths,
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


def _write_executable(path: Path) -> None:
    path.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
    path.chmod(path.stat().st_mode | stat.S_IXUSR)


def test_replay_process_requests_raw_machine_and_scenario_evidence() -> None:
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
            scenario="fixture.json",
            machine_event=lambda _payload: None,
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


def main() -> int:
    test_file_artifact_owns_raw_bytes()
    test_replay_stimulus_is_persisted_as_raw_ndjson_once()
    test_runner_logs_are_confined_to_the_run_directory()
    test_timeline_keeps_ordered_external_events_and_scrubs_private_paths()
    test_replay_process_requests_raw_machine_and_scenario_evidence()
    test_radio_lease_excludes_concurrent_owners_and_rejects_symlink_parent()
    test_runner_source_is_external_only_and_serial_is_read_only()
    print("bench window tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
