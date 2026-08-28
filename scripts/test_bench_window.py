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
    REPLAY_DELIVERY_NAME,
    REPLAY_STIMULUS_NAME,
    BenchTimeline,
    RuntimeIdentityFailure,
    RuntimeIdentityTracker,
    V1Emulator,
    V1RadioLease,
    establish_serial_boundary,
    file_artifact,
    parse_runtime_boot_identity,
    notification_delivery_problem,
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


def main() -> int:
    test_file_artifact_owns_raw_bytes()
    test_replay_stimulus_is_persisted_as_raw_ndjson_once()
    test_replay_delivery_is_persisted_with_explicit_loss_denominators()
    test_delivery_summary_counts_attempts_and_vetoes_dropped_or_skipped()
    test_delivery_summary_vetoes_malformed_sequence_lifecycles()
    test_delivery_summary_vetoes_empty_instrumentation_stream()
    test_runner_logs_are_confined_to_the_run_directory()
    test_timeline_keeps_ordered_external_events_and_scrubs_private_paths()
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
    test_serial_boundary_waits_for_attach_time_boot_past_initial_observation()
    test_missing_and_malformed_boot_identity_fail()
    test_conflicting_boot_identities_fail()
    test_serial_boundary_fails_if_detected_startup_never_reaches_boot_identity()
    print("bench window tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
