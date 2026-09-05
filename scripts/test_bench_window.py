#!/usr/bin/env python3
"""Focused regressions for the external-only bench evidence contract."""

from __future__ import annotations

import contextlib
import fcntl
import hashlib
import io
import json
import os
import stat
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


def test_main_writes_collection_only_and_returns_exit_one_for_unlinked_no_flash() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        out_dir = Path(tmp)
        args = SimpleNamespace(
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
                          encounter_result: str = "PASS", camera: bool = True,
                          camera_present: bool = True, visual_exit: int = 0,
                          encounter_exit: int | None = None,
                          encounter_payload: dict | None = None,
                          write_encounter_result: bool = True,
                          interrupt_encounter: bool = False,
                          run_all: bool = False,
                          qualification_capture: bool = False,
                          capture_records: list[dict | None] | None = None,
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
        fields = ("counter_glyph", "primary_frequency", "active_bands", "main_arrows",
                  "main_bars", "secondary", "muted_badge")
        field_status = {"PASS": "MATCH", "FAIL": "DIFFERENCE", "INCONCLUSIVE": "UNRESOLVED"}[encounter_result]
        samples = [{"frame_id": f"{number:04d}", "comparison": {"checks": {
            field: {"status": (field_status if number == 2 and field == "primary_frequency" else "MATCH"),
                    "reason": "fixture frequency observation"}
            for field in fields}, "joint_state": {"status": "MATCH"}}} for number in (1, 2)]
        field_counts = {"MATCH": 14} if encounter_result == "PASS" else {"MATCH": 13, field_status: 1}
        encounter = encounter_payload if encounter_payload is not None else {
            "kind": "sampled_encounter_check", "result": encounter_result,
            "counts": {"required": 14, "fields": field_counts, "joint_states": {"MATCH": 2}},
            "samples": samples, "errors": [],
            "coverage": {"requests": 2, "selected_unique_frames": 2, "unique_frames": 2,
                         "regions": [{"maximum_unobserved_gap_seconds": .5}]},
        }
        if isinstance(encounter, dict) and "counts" in encounter:
            raw_result = encounter.get("result", encounter_result)
            encounter.setdefault("raw_frame_result", raw_result)
            product_reason = {"PASS": "ALL_REQUIRED_EVENTS_PASSED",
                              "FAIL": "TARGET_LATE",
                              "INCONCLUSIVE": "UNCLASSIFIED_VISIBLE_INTERVAL"}.get(raw_result,
                                                                                     "NO_REQUIRED_EVENTS")
            point_id = encounter["samples"][-1]["frame_id"] if encounter.get("samples") else "0001"
            event = {"event_id": "event-0001", "result": raw_result,
                     "reason_code": product_reason,
                     "first_decisive_marker": {"frame_id": point_id}}
            product_counts = {"required_events": 1,
                              "passed": int(raw_result == "PASS"),
                              "failed": int(raw_result == "FAIL"),
                              "inconclusive": int(raw_result == "INCONCLUSIVE")}
            encounter.setdefault("reader_qualification", {"schema_version": 1,
                                                           "kind": "encounter_reader_qualification_verification",
                                                           "status": "QUALIFIED", "errors": []})
            encounter.setdefault("primary_judgment", {
                "schema_version": 1, "kind": "visible_event_presentation",
                "contract": {"id": "VISIBLE_EVENT_PRESENTATION", "version": 2,
                             "clock": "host_monotonic_capture_marker",
                             "anchor": "first_complete_target_input_all_accepted_ns",
                             "appearance_deadline_ns": 100_000_000,
                             "appearance_decision_rule":
                                 "first_source_marker_at_or_after_nominal_deadline",
                             "maximum_appearance_observation_bracket_ns": 10_000_000,
                             "verification_duration_ns": 192_000_000,
                             "maximum_source_marker_gap_ns": 10_000_000,
                             "minimum_post_completion_hold_ns": 312_000_000},
                "execution": {"status": "COMPLETE", "fatal_integrity_errors": [],
                              "temporal_classification_errors": []},
                "events": [event], "counts": product_counts,
                "result": raw_result, "reason_code": product_reason})
        encounter_path.write_text(json.dumps(encounter), encoding="utf-8")
        counter_marker = root / "counter.calls"
        encounter_marker = root / "encounter.calls"
        visual_marker = root / "visual.calls"

        python = fake_bin / "python3"
        python.write_text(textwrap.dedent(f"""\
            #!/usr/bin/env bash
            set -uo pipefail
            fixture_root="$(cd "$(dirname "$0")/.." && pwd)"
            if [[ "${{1:-}}" == "-c" ]]; then printf 'fixture\\n'; exit 0; fi
            if [[ "${{1:-}}" == "-" ]]; then exec {sys.executable} "$@"; fi
            if [[ "${{1:-}}" == */scripts/bench/run_logged.py ]]; then
              if [[ " $* " == *"/tools/v1replay/scripts/build.sh"* ]]; then
                mkdir -p "$fixture_root/tools/v1replay/.build"
                printf '#!/bin/sh\\nexit 0\\n' > "$fixture_root/tools/v1replay/.build/v1replay"
                chmod +x "$fixture_root/tools/v1replay/.build/v1replay"
                exit 0
              fi
              if [[ " $* " == *"/scripts/bench/run_window.py"* ]]; then
                args=("$@")
                for ((index=0; index<${{#args[@]}}; index++)); do
                  if [[ "${{args[index]}}" == "--out-dir" ]]; then out="${{args[index+1]}}"; fi
                done
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
              transition_review=0
              reader_qualification=0
              for ((index=0; index<${{#args[@]}}; index++)); do
                if [[ "${{args[index]}}" == "--out" ]]; then out="${{args[index+1]}}"; fi
                if [[ "${{args[index]}}" == "--run-dir" ]]; then run="${{args[index+1]}}"; fi
                if [[ "${{args[index]}}" == "--inspect-transitions" ]]; then transition_review=1; fi
                if [[ "${{args[index]}}" == "--reader-qualification" ]]; then reader_qualification=1; fi
              done
              [[ "$transition_review" == 1 && "$reader_qualification" == 1 ]] || exit 9
              [[ -e "$run/window_result.json" && ! -e "$out" ]] || exit 9
              mkdir -p "$out"
              if [[ "$FAKE_ENCOUNTER_WRITE" == 1 ]]; then
                cp "$FAKE_ENCOUNTER_JSON" "$out/result.json"
                printf '<title>fixture encounter report</title>\\n' > "$out/report.html"
              fi
              printf 'called\\n' >> "$FAKE_ENCOUNTER_MARKER"
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
        device = root / "device"
        device.touch()
        environment = dict(os.environ)
        environment.update(
            PATH=str(fake_bin) + os.pathsep + environment.get("PATH", ""),
            DEVICE_PORT=str(device),
            BENCH_ARTIFACT_ROOT=str(root / "artifacts"),
            BENCH_DURATION_SECONDS="1",
            BENCH_REPLAY_DURATION_SECONDS="1",
            BENCH_POST_UPLOAD_SETTLE_SECONDS="0",
            FAKE_WINDOW_JSON=str(window_path),
            FAKE_WINDOW_EXIT=str(window_status),
            FAKE_COUNTER_JSON=str(counter_path),
            FAKE_COUNTER_EXIT=str({"PASS": 0, "FAIL": 1, "INCONCLUSIVE": 2}[counter_result]),
            FAKE_COUNTER_MARKER=str(counter_marker),
            FAKE_ENCOUNTER_JSON=str(encounter_path),
            FAKE_ENCOUNTER_EXIT=str(encounter_exit if encounter_exit is not None else {"PASS": 0, "FAIL": 1, "INCONCLUSIVE": 2}[encounter_result]),
            FAKE_ENCOUNTER_MARKER=str(encounter_marker),
            FAKE_ENCOUNTER_WRITE=str(int(write_encounter_result)),
            FAKE_ENCOUNTER_INTERRUPT=str(int(interrupt_encounter)),
            FAKE_VISUAL_MARKER=str(visual_marker),
            FAKE_VISUAL_EXIT=str(visual_exit),
        )
        arguments = [str(bench), "--all" if run_all else "--replay", "--no-flash"]
        if camera:
            arguments.append("--camera")
        if qualification_capture:
            arguments.append("--qualification-capture")
        process = subprocess.run(arguments, cwd=root, env=environment, capture_output=True, text=True)
        counter_calls = len(counter_marker.read_text().splitlines()) if counter_marker.exists() else 0
        encounter_calls = len(encounter_marker.read_text().splitlines()) if encounter_marker.exists() else 0
        visual_calls = len(visual_marker.read_text().splitlines()) if visual_marker.exists() else 0
        if capture_records is not None:
            records = list((root / "artifacts").glob("*/runs/*/replay/qualification_capture.json"))
            capture_records.append(json.loads(records[0].read_text(encoding="utf-8")) if len(records) == 1 else None)
        return process, counter_calls, encounter_calls, visual_calls


def test_bench_cli_propagates_encounter_verdicts_with_fixed_precedence() -> None:
    cases = [
        ("PASS", "PASS", 0, "PASS (visible encounter product)"),
        ("PASS", "INCONCLUSIVE", 1, "INCONCLUSIVE (visible encounter product)"),
        ("PASS", "FAIL", 2, "FAIL (visible encounter product)"),
        ("COLLECTION_ONLY", "PASS", 1, "COLLECTION-ONLY (unqualified:"),
        ("COLLECTION_ONLY", "INCONCLUSIVE", 1, "COLLECTION-ONLY (unqualified:"),
        ("COLLECTION_ONLY", "FAIL", 1, "COLLECTION-ONLY (unqualified:"),
    ]
    for window, encounter, status, final in cases:
        process, counter_calls, encounter_calls, _ = run_bench_cli_fixture(window, "PASS", encounter_result=encounter, visual_exit=2)
        assert_true(process.returncode == status, f"{window}/{encounter}: {process.stdout} {process.stderr}")
        assert_true((counter_calls, encounter_calls) == (1, 1), process.stdout)
        assert_true("[bench] sampled live counter: PASS |" in process.stdout, process.stdout)
        assert_true(f"[bench] visible encounter product: {encounter} |" in process.stdout, process.stdout)
        assert_true("2 requests | 2 selected, 2 decoded original frames | largest unobserved gap 0.500s" in process.stdout, process.stdout)
        assert_true("host input acceptance: 10 / 10 packets" in process.stdout, process.stdout)
        assert_true("DUT receipt not observed" in process.stdout, process.stdout)
        assert_true("encounter-check/report.html" in process.stdout, process.stdout)
        if encounter != "PASS":
            assert_true("encounter-check/report.html#sample=0002" in process.stdout, process.stdout)
            assert_true("event-0001:" in process.stdout, process.stdout)
        assert_true(final in process.stdout.splitlines()[-1], process.stdout)

    process, counter_calls, encounter_calls, visual_calls = run_bench_cli_fixture("PASS", "FAIL", visual_exit=2, run_all=True)
    assert_true(process.returncode == 0, process.stdout)
    assert_true((counter_calls, encounter_calls, visual_calls) == (1, 1, 0), process.stdout)
    assert_true("[bench] sampled live counter: FAIL |" in process.stdout, process.stdout)
    assert_true("visual timing" not in process.stdout, process.stdout)


def test_bench_cli_preserves_non_camera_and_hard_collection_results() -> None:
    process, counter_calls, encounter_calls, visual_calls = run_bench_cli_fixture("PASS", "PASS", camera=False)
    assert_true(process.returncode == 0, process.stdout)
    assert_true((counter_calls, encounter_calls, visual_calls) == (0, 0, 0), process.stdout)
    assert_true("[bench] sampled live counter: NOT_EVALUATED" in process.stdout, process.stdout)
    assert_true("[bench] visible encounter product: NOT_EVALUATED" in process.stdout, process.stdout)
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
    assert_true("visible encounter product: NOT_EVALUATED" in process.stdout, process.stdout)
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
    process, counter_calls, encounter_calls, visual_calls = run_bench_cli_fixture("PASS", "PASS", camera_present=False)
    assert_true(process.returncode == 1, process.stdout)
    assert_true((counter_calls, encounter_calls, visual_calls) == (0, 0, 0), process.stdout)
    assert_true("visible encounter product: INCONCLUSIVE | requested camera evidence is unavailable" in process.stdout, process.stdout)

    for options in (
        {"write_encounter_result": False, "encounter_exit": 130},
        {"encounter_exit": 2},
        {"encounter_payload": {"result": "PASS"}},
        {"interrupt_encounter": True},
    ):
        process, counter_calls, encounter_calls, _ = run_bench_cli_fixture("PASS", "PASS", **options)
        assert_true(process.returncode == 1, f"{options}: {process.stdout} {process.stderr}")
        assert_true((counter_calls, encounter_calls) == (1, 1), process.stdout)
        assert_true("visible encounter product: INCONCLUSIVE" in process.stdout, process.stdout)
        assert_true(process.stdout.splitlines()[-1] == "INCONCLUSIVE (visible encounter product)", process.stdout)


def test_bench_cli_preserves_joint_state_failures_and_vetoes_incomplete_pass() -> None:
    payload = {
        "kind": "sampled_encounter_check", "result": "FAIL", "errors": [],
        "counts": {"required": 7, "fields": {"MATCH": 7}, "joint_states": {"DIFFERENCE": 1}},
        "coverage": {"requests": 1, "selected_unique_frames": 1, "unique_frames": 1,
                     "regions": [{"maximum_unobserved_gap_seconds": .5}]},
        "samples": [{"frame_id": "0001", "comparison": {
            "checks": {name: {"status": "MATCH"} for name in (
                "counter_glyph", "primary_frequency", "active_bands", "main_arrows",
                "main_bars", "secondary", "muted_badge")},
            "joint_state": {"status": "DIFFERENCE", "reason": "fixture mixed blink phases"}}}],
    }
    process, _, _, _ = run_bench_cli_fixture("PASS", "PASS", encounter_result="FAIL", encounter_payload=payload)
    assert_true(process.returncode == 2, process.stdout)
    assert_true("0 passed, 1 failed, 0 inconclusive / 1 required visible events" in process.stdout, process.stdout)
    assert_true("event-0001: TARGET_LATE" in process.stdout, process.stdout)
    assert_true("report.html#sample=0001" in process.stdout, process.stdout)

    payload["result"] = "PASS"
    for decoded in (1, 0):
        payload["coverage"]["unique_frames"] = decoded
        if decoded == 0:
            payload["counts"]["joint_states"] = {"MATCH": 1}
            payload["samples"][0]["comparison"]["joint_state"] = {"status": "MATCH"}
        process, _, _, _ = run_bench_cli_fixture("PASS", "PASS", encounter_payload=payload)
        assert_true(process.returncode == 1, process.stdout)
        assert_true("visible encounter product: INCONCLUSIVE" in process.stdout, process.stdout)


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


def test_explicit_boundary_requires_fresh_usb_reset_and_refuses_intervening_failure() -> None:
    rom = "ESP-ROM:esp32s3-20210327"
    reason = "rst:0x15 (USB_UART_CHIP_RESET),boot:0xa (SPI_FAST_FLASH_BOOT)"
    boot = "BOOT bootId=4 uptimeMs=2053 reset=USB git=2f32dda image=04904e028"
    ready = "[Boot] Ready gate opened at 2380 ms"
    complete = "[Boot] setup total: 2262 ms"
    cases = [
        ([rom, reason, boot, ready, complete], None),
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


def main() -> int:
    test_file_artifact_owns_raw_bytes()
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
    test_requested_accepted_complete_stopped_is_completed()
    test_radio_lease_excludes_concurrent_owners_and_rejects_symlink_parent()
    test_runner_source_is_external_only_and_serial_is_read_only()
    test_upload_exact_match_is_qualified()
    test_upload_git_mismatch_fails()
    test_upload_image_mismatch_fails()
    test_no_flash_git_match_with_linked_resident_artifact_is_qualified()
    test_no_flash_git_match_with_unlinked_resident_artifact_is_collection_only()
    test_no_flash_git_mismatch_fails()
    test_main_writes_collection_only_and_returns_exit_one_for_unlinked_no_flash()
    test_dirty_source_vetoes_qualification_before_collection()
    test_top_level_pass_is_vetoed_by_delivery_loss_counters()
    test_top_level_pass_is_vetoed_by_malformed_delivery_sequence()
    test_top_level_pass_is_vetoed_by_empty_delivery_stream()
    test_clean_source_preserves_qualified_pass_behavior()
    test_bench_cli_collection_only_branch_has_no_pass_verdict()
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
    print("bench window tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
