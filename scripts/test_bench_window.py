#!/usr/bin/env python3
"""Focused regressions for the external-only bench evidence contract."""

from __future__ import annotations

import contextlib
import fcntl
import hashlib
import inspect
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
    PRESENTATION_SNAPSHOT_NAME,
    REPLAY_DISPLAY_CONTRACT_NAME,
    REPLAY_DELIVERY_NAME,
    REPLAY_STIMULUS_NAME,
    BenchTimeline,
    RuntimeIdentityFailure,
    RuntimeIdentityTracker,
    SourceProvenanceFailure,
    V1Emulator,
    V1RadioLease,
    capture_presentation_configuration,
    establish_serial_boundary,
    file_artifact,
    parse_runtime_boot_identity,
    parse_auto_push_selection,
    publish_presentation_snapshot,
    publish_replay_display_contract,
    publish_replay_delivery_evidence,
    publish_replay_stimulus_evidence,
    qualify_runtime_identity,
    require_current_source_identity,
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


def test_build_artifacts_retain_exact_application_after_build_cache_changes() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        recording = root / "recording"
        build = root / "build-cache"
        recording.mkdir()
        build.mkdir()
        for name in run_window_module.BUILD_UPLOAD_FILES:
            (build / name).write_bytes(("synthetic build output: " + name).encode())
        original = build / "firmware.bin"
        original_bytes = original.read_bytes()
        result = run_window_module.retain_build_upload_artifacts(
            recording, build, upload_performed=True
        )
        manifest = json.loads(
            (recording / run_window_module.BUILD_UPLOAD_ARTIFACTS_NAME).read_text()
        )
        record = next(item for item in manifest["files"] if item["name"] == "firmware.bin")
        retained = recording / record["path"]
        assert_true(record["path"] == "firmware.bin", str(record))
        assert_true(record["size_bytes"] == len(original_bytes), str(record))
        assert_true(record["sha256"] == hashlib.sha256(original_bytes).hexdigest(), str(record))
        assert_true(
            file_artifact(retained)
            == {key: record[key] for key in ("path", "size_bytes", "sha256")},
            str(record),
        )
        assert_true(not original.samefile(retained), "retained application aliases build output")
        assert_true(result["schema_version"] == 1 and result["missing"] == [], str(result))
        assert_true(result["upload_scope"] == "platformio_upload_target" and
                    result["filesystem_upload_requested"] is False, str(result))
        original.write_bytes(b"next build replaced this image")
        assert_true(retained.read_bytes() == original_bytes, "later build changed retained application")


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
                 "--suite", "replay", "--out-dir", str(out_dir),
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


def capture_replay_command(
    scenario: str,
    *,
    ku_qualification: bool = False,
    photo_label_qualification: bool = False,
    junk_qualification: bool = False,
) -> list[str]:
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
            ku_qualification=ku_qualification,
            machine_event=lambda _payload: None,
            photo_label_qualification=photo_label_qualification,
            junk_qualification=junk_qualification,
        )
        try:
            emulator.start()
        finally:
            if emulator.log_handle is not None:
                emulator.log_handle.close()
            run_window_module.subprocess.Popen = original

        assert_true("--machine-events" in captured, str(captured))
        assert_true("--state-file" in captured, str(captured))
        state_index = captured.index("--state-file") + 1
        assert_true(captured[state_index].endswith("v1_emulator_state.json"), str(captured))
        assert_true("--scenario-evidence" in captured, str(captured))
        assert_true("--owner-pid" in captured, str(captured))
        return captured


def test_replay_process_requests_raw_machine_and_scenario_evidence() -> None:
    for scenario in ("fixture.json", ""):
        command = capture_replay_command(scenario)
        assert_true(("--scenario" in command) is bool(scenario), str(command))
    ku_command = capture_replay_command("", ku_qualification=True)
    assert_true("--ku-qualification" in ku_command, str(ku_command))
    photo_command = capture_replay_command("", photo_label_qualification=True)
    assert_true("--photo-label-qualification" in photo_command, str(photo_command))
    junk_command = capture_replay_command("", junk_qualification=True)
    assert_true("--junk-qualification" in junk_command, str(junk_command))


def test_replay_transport_must_be_active_before_the_external_window() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        out_dir = Path(tmp)
        executable = out_dir / "v1replay"
        _write_executable(executable)
        emulator = V1Emulator(
            executable,
            out_dir,
            "replay",
            "scenario",
            lease_fd=9,
            scenario="",
            ku_qualification=False,
            machine_event=lambda _payload: None,
        )
        emulator.process = SimpleNamespace(poll=lambda: None)
        emulator.log_path.write_text(
            'V1REPLAY_EVENT {"state":"session_transport","active":false}\n',
            encoding="utf-8",
        )
        try:
            emulator.wait_for_transport(0.001)
        except RuntimeError as exc:
            assert_true("before the external window" in str(exc), str(exc))
        else:
            raise AssertionError("replay external window started without active V1 transport")

        emulator.log_path.write_text(
            'V1REPLAY_EVENT {"state":"session_transport","active":true}\n',
            encoding="utf-8",
        )
        emulator.observed_events = 0
        emulator.wait_for_transport(0.1)


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
            ku_qualification=False,
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


def test_requested_dropped_complete_stopped_preserves_raw_delivery() -> None:
    result = finish_replay_fixture(
        ["notification_requested", "notification_dropped", "complete", "stopped"]
    )
    assert_true(result["lifecycle_completed"] is True, str(result))
    assert_true(
        [event["state"] for event in result["delivery_events"]]
        == ["notification_requested", "notification_dropped"],
        str(result),
    )


def test_emulator_cleanup_before_start_preserves_primary_failure() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        emulator = V1Emulator(Path("unused"), Path(tmp), "replay", "scenario", lease_fd=9, scenario="",
                              ku_qualification=False, machine_event=lambda _payload: None)
        result = emulator.finish(window_completed=False)
        assert_true(result["started"] is False, str(result))
        assert_true(result["lifecycle_completed"] is False, str(result))
        assert_true(not emulator.log_path.exists(), "cleanup invented replay evidence")
        emulator.process = SimpleNamespace(poll=lambda: 0)
        try:
            emulator.finish(window_completed=False)
        except FileNotFoundError:
            pass
        else:
            raise AssertionError("missing started-emulator evidence was suppressed")


def test_requested_accepted_complete_stopped_preserves_raw_delivery() -> None:
    result = finish_replay_fixture(
        ["notification_requested", "notification_accepted", "complete", "stopped"]
    )
    assert_true(result["lifecycle_completed"] is True, str(result))
    assert_true(
        [event["state"] for event in result["delivery_events"]]
        == ["notification_requested", "notification_accepted"],
        str(result),
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


def test_raw_bench_entrypoint_returns_complete_without_grading_artifacts() -> None:
    source = (ROOT / "bench.sh").read_text(encoding="utf-8")
    assert_true("printf 'COMPLETE: %s\\n'" in source,
                "raw bench does not report collection completion")
    assert_true(
        "--terminal-prefix '[bench]'" in source,
        "raw bench hides its managed collection progress",
    )
    runner_source = (ROOT / "scripts" / "bench" / "run_window.py").read_text(
        encoding="utf-8"
    )
    assert_true(
        "[bench] external window complete; finalizing raw evidence" in runner_source,
        "raw bench does not explain the post-window finalization delay",
    )
    assert_true(
        "[bench] raw evidence finalized" in runner_source,
        "raw bench does not report finalization completion",
    )
    assert_true(
        "detect_usb_port" not in source and "device list" not in source,
        "raw bench performs serial discovery before maintenance HTTP capture",
    )
    for retired in (
        "encounter_check",
        "counter_check",
        "visual_run_check",
        "analyze-recording",
        "replay_stimulus\"][",
        "notification_delivery",
        "recorder_stats",
    ):
        assert_true(retired not in source, f"raw bench still grades {retired}")


def test_raw_bench_refuses_a_failed_git_status_check() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        temp_root = Path(tmp)
        fake_bin = temp_root / "bin"
        fake_bin.mkdir()
        fake_git = fake_bin / "git"
        fake_git.write_text(
            """#!/bin/sh
if [ "$1" = "rev-parse" ]; then
  case "$2" in
    HEAD) printf '%s\\n' '2f32ddab989792917b5b3df9206d9751ebfd8289' ;;
    --short) printf '%s\\n' '2f32dda' ;;
    --abbrev-ref) printf '%s\\n' 'main' ;;
    *) exit 19 ;;
  esac
  exit 0
fi
if [ "$1" = "status" ]; then
  exit 17
fi
exit 23
""",
            encoding="utf-8",
        )
        fake_git.chmod(0o755)
        environment = {
            **os.environ,
            "PATH": f"{fake_bin}{os.pathsep}{os.environ.get('PATH', '')}",
            "BENCH_ARTIFACT_ROOT": str(temp_root / "artifacts"),
        }
        completed = subprocess.run(
            ["bash", str(ROOT / "bench.sh"), "--replay", "--camera"],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=False,
            env=environment,
        )
        assert_true(completed.returncode == 2, str(completed))
        assert_true(
            "COLLECTION_FAILED: could not inspect the source worktree" in completed.stdout,
            completed.stdout + completed.stderr,
        )


def test_source_identity_check_requires_successful_git_inspection() -> None:
    responses = [
        subprocess.CompletedProcess([], 0, GIT_SHA, ""),
        subprocess.CompletedProcess([], 0, "main\n", ""),
        subprocess.CompletedProcess([], 17, "", "inspection failed"),
    ]
    with mock.patch.object(run_window_module.subprocess, "run", side_effect=responses):
        try:
            require_current_source_identity(
                expected_git_sha=GIT_SHA,
                expected_git_ref="main",
            )
        except SourceProvenanceFailure as exc:
            assert_true(exc.reason == "source_repository_uninspectable", str(exc))
        else:
            raise AssertionError("failed git status was accepted as a clean worktree")


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
        ),
        "does not match uploaded firmware image",
    )
    assert_true(exc.qualification["git_match"] is True, str(exc.qualification))
    assert_true(exc.qualification["image_match"] is False, str(exc.qualification))


def test_missing_upload_record_fails() -> None:
    assert_identity_failure(
        lambda: qualify_runtime_identity(
            dict(RUNTIME_IDENTITY),
            intended_git_sha=GIT_SHA,
            build_upload=build_upload_artifact("04904e028", upload_performed=False),
        ),
        "does not record the required firmware upload",
    )


def test_dirty_source_vetoes_qualification_before_collection() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        out_dir = Path(tmp)
        args = SimpleNamespace(
            camera=False,
            board_id="fixture",
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


def test_clean_source_returns_complete_without_grading_artifacts() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        out_dir = Path(tmp)
        args = SimpleNamespace(
            camera=False,
            board_id="fixture",
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
            "require_current_source_identity": run_window_module.require_current_source_identity,
            "serial": run_window_module.serial,
        }
        source_checks = 0

        def verified_source(**_kwargs: Any) -> None:
            nonlocal source_checks
            source_checks += 1

        run_window_module.parse_args = lambda: args  # type: ignore[assignment]
        run_window_module.install_signal_handlers = lambda: None  # type: ignore[assignment]
        run_window_module.collect_live = (  # type: ignore[assignment]
            lambda _args, _out_dir, _artifacts: collected
        )
        run_window_module.require_current_source_identity = verified_source  # type: ignore[assignment]
        run_window_module.serial = object()  # type: ignore[assignment]
        try:
            status = run_window_module.main()
        finally:
            for name, value in originals.items():
                setattr(run_window_module, name, value)

        payload = json.loads((out_dir / "window_result.json").read_text(encoding="utf-8"))
        assert_true(status == 0, f"clean qualified exit changed: {status}")
        assert_true(payload["result"] == "COMPLETE", str(payload))
        assert_true(source_checks == 2, f"source was checked {source_checks} times")


def test_source_change_after_collection_vetoes_completion() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        out_dir = Path(tmp)
        args = SimpleNamespace(
            camera=False,
            board_id="fixture",
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
        source_checks = iter(
            [
                None,
                SourceProvenanceFailure(
                    "source worktree changed during raw collection",
                    reason="source_worktree_dirty",
                ),
            ]
        )
        originals = {
            "parse_args": run_window_module.parse_args,
            "install_signal_handlers": run_window_module.install_signal_handlers,
            "collect_live": run_window_module.collect_live,
            "require_current_source_identity": run_window_module.require_current_source_identity,
            "serial": run_window_module.serial,
        }

        def verify_source(**_kwargs: Any) -> None:
            outcome = next(source_checks)
            if outcome is not None:
                raise outcome

        run_window_module.parse_args = lambda: args  # type: ignore[assignment]
        run_window_module.install_signal_handlers = lambda: None  # type: ignore[assignment]
        run_window_module.collect_live = (  # type: ignore[assignment]
            lambda _args, _out_dir, _artifacts: collected
        )
        run_window_module.require_current_source_identity = verify_source  # type: ignore[assignment]
        run_window_module.serial = object()  # type: ignore[assignment]
        try:
            status = run_window_module.main()
        finally:
            for name, value in originals.items():
                setattr(run_window_module, name, value)

        payload = json.loads((out_dir / "window_result.json").read_text(encoding="utf-8"))
        assert_true(status == 2, f"changed source exit changed: {status}")
        assert_true(payload["result"] == "FAIL", str(payload))
        assert_true(payload["failure_kind"] == "source_provenance", str(payload))
        assert_true(
            payload["runtime_qualification"]["reason"] == "source_worktree_dirty",
            str(payload),
        )


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
            assert_true(observer.read_count == len(lines),
                        "boundary returned before normal setup completed")
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
    cases += [(prefix + rom + rom, suffix, None, "unexpected or repeated ROM start")
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
    cases += [(prefix + rom, suffix, None, "unexpected or repeated ROM start")
              for prefix in invalid_prefixes if prefix]
    cases += [
        ("load:0x3fce2" + rom + rom, suffix, None, "unexpected or repeated ROM start"),
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


def sample_presentation_api():
    display = {field: index for index, field in enumerate(run_window_module.DISPLAY_DIRECT_FIELDS)}
    display.update({
        "bandPhoto": 0x780F,
        "freqUseBandColor": False,
        "hideProfileIndicator": False,
        "brightness": 170,
    })
    quiet = {
        "alertVolumeFadeEnabled": False,
        "alertVolumeFadeDelaySec": 3,
        "alertVolumeFadeVolume": 3,
        "speedMuteEnabled": False,
        "speedMuteThresholdMph": 15,
        "speedMuteHysteresisMph": 3,
        "speedMuteVolume": 0,
        "speedMuteVoice": False,
        "stealthEnabled": False,
    }
    slots = {
        "enabled": True,
        "activeSlot": 0,
        "schemaVersion": 3,
        "detectorConfigurationOwner": "profile",
        "slots": [
            {"name": "MrCt", "profile": "Advanced Logic", "color": 0x780F,
             "alertPersist": 2, "priorityArrowOnly": False},
            {"name": "P", "profile": "Advanced Logic", "color": 0x780F,
             "alertPersist": 0, "priorityArrowOnly": True},
            {"name": "Road", "profile": "Road", "color": 0x07E0,
             "alertPersist": 1, "priorityArrowOnly": False},
        ],
    }
    calls = []

    def get_json(_base, endpoint, *, query=None):
        calls.append((endpoint, query))
        if endpoint == "/api/display/settings":
            return display
        if endpoint == "/api/quiet/settings":
            return quiet
        if endpoint == "/api/autopush/slots":
            return slots
        raise AssertionError(endpoint)

    return get_json, calls


def test_presentation_capture_retains_visual_inputs_without_private_profile_names() -> None:
    get_json, calls = sample_presentation_api()
    captured = capture_presentation_configuration("http://device", get_json=get_json)
    slots = captured["auto_push"]["slots"]
    encoded = json.dumps(captured, sort_keys=True)
    assert_true(captured["display"]["bandPhoto"] == 0x780F, encoded)
    assert_true(slots[0]["display_label"] == "MrCt" and not slots[0]["display_label_is_opaque"], encoded)
    assert_true("Advanced Logic" not in encoded, encoded)
    assert_true(not any(endpoint == "/api/v1/profile" for endpoint, _ in calls), str(calls))


def test_presentation_capture_accepts_unassigned_slot_but_rejects_malformed_slot() -> None:
    get_json, _ = sample_presentation_api()
    slots = get_json("http://device", "/api/autopush/slots")
    slots["slots"][1]["profile"] = ""
    for enabled in (True, False):
        slots["enabled"] = enabled
        captured = capture_presentation_configuration("http://device", get_json=get_json)
        assert_true(captured["auto_push"]["enabled"] is enabled, str(captured))
        assert_true(len(captured["auto_push"]["slots"]) == 3, str(captured))

    for malformed in (None, 42):
        slots["slots"][1]["profile"] = malformed
        try:
            capture_presentation_configuration("http://device", get_json=get_json)
        except RuntimeError as error:
            assert_true("Auto-Push slot response is invalid" in str(error), str(error))
        else:
            raise AssertionError(f"malformed slot profile {malformed!r} was accepted")
    del slots["slots"][1]["profile"]
    try:
        capture_presentation_configuration("http://device", get_json=get_json)
    except RuntimeError as error:
        assert_true("Auto-Push slot response is invalid" in str(error), str(error))
    else:
        raise AssertionError("missing slot profile was accepted")


def test_presentation_selection_intent_is_explicit() -> None:
    selection = parse_auto_push_selection(
        "[AutoPush] onV1Connected autoPush=on activeSlot=0 selectedSlot=2 defaultProfile=3 mode=1"
    )
    assert_true(selection is not None and selection["intended_detector_profile_slot"] == 2, str(selection))
    assert_true(selection["presentation_policy_slot"] == 0, str(selection))
    disabled = parse_auto_push_selection(
        "[AutoPush] onV1Connected autoPush=off activeSlot=0 selectedSlot=2 defaultProfile=3 mode=1"
    )
    assert_true(disabled is not None and disabled["intended_detector_profile_slot"] is None,
                str(disabled))
    try:
        parse_auto_push_selection(
            "[AutoPush] onV1Connected autoPush=on activeSlot=0 selectedSlot=1 defaultProfile=3 mode=1"
        )
    except RuntimeError:
        pass
    else:
        raise AssertionError("inconsistent selected-slot evidence was accepted")


def test_bench_upload_preserves_littlefs_settings() -> None:
    with mock.patch.object(run_window_module.subprocess, "run") as run:
        run_window_module.run_upload("/dev/test", skip_web=False)
    command = run.call_args.args[0]
    assert_true("-u" in command and "-f" not in command, str(command))


def test_bench_joins_maintenance_wifi_without_usb_status_query() -> None:
    responses = iter(
        [
            SimpleNamespace(
                returncode=0,
                stdout="Hardware Port: Wi-Fi\nDevice: en0\nEthernet Address: aa:bb\n\n",
            ),
            SimpleNamespace(
                returncode=0,
                stdout="You are not associated with a Wi-Fi network.\n",
            ),
            SimpleNamespace(returncode=1, stdout=""),
            SimpleNamespace(returncode=0, stdout=""),
            SimpleNamespace(returncode=0, stdout="Current Wi-Fi Network: V1-Simple\n"),
        ]
    )
    calls = []

    def run(command, **_kwargs):
        calls.append(command)
        return next(responses)

    with mock.patch.dict(os.environ, {}, clear=True):
        assert_true(run_window_module.try_join_maintenance_wifi(run=run), str(calls))
    assert_true(
        calls[2]
        == ["/usr/sbin/networksetup", "-setairportnetwork", "en0", "V1-Simple"],
        str(calls),
    )
    assert_true(calls[3][-1] == "setupv1simple", str(calls))
    assert_true(not any("usb_profiles" in part for command in calls for part in command), str(calls))


def test_bench_retries_wifi_join_during_http_wait() -> None:
    clock = [0.0]
    reconnect_calls = []

    def reconnect(_remaining: float) -> bool:
        reconnect_calls.append(clock[0])
        return len(reconnect_calls) >= 2

    def capture(_base_url: str, **_kwargs: Any) -> dict[str, Any]:
        if len(reconnect_calls) >= 2:
            return {"captured": True}
        raise run_window_module.PresentationConnectionFailure("not ready")

    result = run_window_module.wait_for_presentation_configuration(
        "http://device",
        timeout_s=20,
        monotonic=lambda: clock[0],
        sleep=lambda seconds: clock.__setitem__(0, clock[0] + seconds),
        capture=capture,
        reconnect=reconnect,
    )
    assert_true(result == {"captured": True}, str(result))
    assert_true(reconnect_calls == [0.0, 15.0], str(reconnect_calls))


def test_wifi_join_uses_shared_deadline() -> None:
    clock = [0.0]
    command_timeouts = []

    def run(command, **kwargs):
        timeout = kwargs["timeout"]
        command_timeouts.append(timeout)
        clock[0] += timeout
        if command[-1] == "-listallhardwareports":
            return SimpleNamespace(
                returncode=0,
                stdout="Hardware Port: Wi-Fi\nDevice: en0\n",
            )
        return SimpleNamespace(
            returncode=1,
            stdout="You are not associated with a Wi-Fi network.\n",
        )

    joined = run_window_module.try_join_maintenance_wifi(
        run=run,
        timeout_s=25.0,
        monotonic=lambda: clock[0],
    )
    assert_true(not joined, str(command_timeouts))
    assert_true(clock[0] == 25.0, str((clock[0], command_timeouts)))
    assert_true(sum(command_timeouts) == 25.0, str(command_timeouts))


def test_presentation_http_is_maintenance_only_and_precedes_upload() -> None:
    source = inspect.getsource(run_window_module.collect_live)
    capture = source.index("wait_for_presentation_configuration(")
    serial_discovery = source.index("wait_for_port(")
    upload = source.index("run_upload(")
    assert_true(capture < serial_discovery < upload,
                "presentation capture must precede serial discovery and normal-mode upload")
    assert_true(source.count("wait_for_presentation_configuration(") == 1,
                "normal-mode collection must not contact the maintenance HTTP API")
    assert_true("ensure_maintenance_usb_status" not in source and "usb_profiles" not in source,
                "serial USB queries must follow the maintenance HTTP snapshot")


def test_display_contract_binds_snapshot_stimulus_scenario_runtime_and_camera() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        out_dir = Path(tmp)
        get_json, _ = sample_presentation_api()
        configuration = capture_presentation_configuration("http://device", get_json=get_json)
        selection = parse_auto_push_selection(
            "[AutoPush] onV1Connected autoPush=on activeSlot=0 selectedSlot=2 defaultProfile=3 mode=1"
        )
        assert selection is not None
        snapshot = publish_presentation_snapshot(
            out_dir, configuration=configuration,
            selection=selection,
            maintenance_capture_ns=10,
        )
        snapshot_payload = json.loads((out_dir / PRESENTATION_SNAPSHOT_NAME).read_text())
        assert_true(snapshot_payload["http_source_mode_declared"] == "maintenance", str(snapshot_payload))
        assert_true(snapshot_payload["serial_port_opened_before_capture"] is False,
                    str(snapshot_payload))
        assert_true(snapshot_payload["http_same_dut_as_normal_runtime_proven"] is False,
                    str(snapshot_payload))
        assert_true(snapshot_payload["full_normal_ram_match_proven"] is False, str(snapshot_payload))
        files = {
            "replay_stimulus": (
                REPLAY_STIMULUS_NAME,
                b'{"state":"stimulus_requested","schemaVersion":4,"stimulusSequence":1,'
                b'"expected":{"phase":"test","alerts":[]},"notifications":[]}\n',
            ),
            "replay_delivery": (REPLAY_DELIVERY_NAME, b'{"schema_version":1,"stimulus_sequence":1,"state":"accepted"}\n'),
            "replay_scenario": (run_window_module.REPLAY_SCENARIO_EVIDENCE_NAME,
                                b'{"schemaVersion":2,"origin":"synthetic_bench","samples":[]}\n'),
            "v1_emulator_state": ("v1_emulator_state.json",
                                  b'{"schemaVersion":1,"userBytes":[127,255,255,255,255,255]}\n'),
            "bench_timeline": (BENCH_TIMELINE_NAME, b'{"event":"done"}\n'),
            "build_upload": (run_window_module.BUILD_UPLOAD_ARTIFACTS_NAME, b'{}\n'),
        }
        artifacts = {"presentation_snapshot": snapshot}
        for key, (name, data) in files.items():
            path = out_dir / name
            path.write_bytes(data)
            artifacts[key] = file_artifact(path)
        artifacts["replay_stimulus"]["event_count"] = 1
        camera_dir = out_dir / "camera"
        camera_dir.mkdir()
        (camera_dir / "capture_manifest.json").write_text('{"kind":"bench_camera_capture"}\n')
        contract = publish_replay_display_contract(
            out_dir,
            artifacts=artifacts,
            runtime_identity={"boot_id": 7, "git_sha": "a" * 40, "image_id": "b" * 9},
            camera_result={"capture_manifest": "capture_manifest.json", "capture_id": "c" * 64},
        )
        payload = json.loads((out_dir / REPLAY_DISPLAY_CONTRACT_NAME).read_text())
        assert_true(contract["sha256"] == file_artifact(out_dir / REPLAY_DISPLAY_CONTRACT_NAME)["sha256"], str(contract))
        assert_true(payload["presentation_snapshot"]["sha256"] == snapshot["sha256"], str(payload))
        assert_true(payload["expected_display_rubric"]["event_count"] == 1, str(payload))
        assert_true(payload["notification_delivery"]["path"] == REPLAY_DELIVERY_NAME, str(payload))
        assert_true(payload["terminal_persisted_emulator_state"]["path"] == "v1_emulator_state.json", str(payload))
        assert_true(payload["runtime"]["boot_id"] == 7 and payload["camera"]["capture_id"] == "c" * 64,
                    str(payload))


def main() -> int:
    test_file_artifact_owns_raw_bytes()
    test_build_artifacts_retain_exact_application_after_build_cache_changes()
    test_build_application_retention_refuses_missing_empty_changed_or_failed_copy()
    test_live_collection_refuses_a_retained_application_before_starting()
    test_reused_live_output_refusal_preserves_existing_evidence()
    test_replay_stimulus_is_persisted_as_raw_ndjson_once()
    test_replay_delivery_is_persisted_with_explicit_loss_denominators()
    test_runner_logs_are_confined_to_the_run_directory()
    test_timeline_keeps_ordered_external_events_and_scrubs_private_paths()
    test_replay_process_requests_raw_machine_and_scenario_evidence()
    test_replay_transport_must_be_active_before_the_external_window()
    test_requested_dropped_complete_stopped_preserves_raw_delivery()
    test_emulator_cleanup_before_start_preserves_primary_failure()
    test_requested_accepted_complete_stopped_preserves_raw_delivery()
    test_radio_lease_excludes_concurrent_owners_and_rejects_symlink_parent()
    test_runner_source_is_external_only_and_serial_is_read_only()
    test_raw_bench_entrypoint_returns_complete_without_grading_artifacts()
    test_raw_bench_refuses_a_failed_git_status_check()
    test_source_identity_check_requires_successful_git_inspection()
    test_upload_exact_match_is_qualified()
    test_upload_git_mismatch_fails()
    test_upload_image_mismatch_fails()
    test_missing_upload_record_fails()
    test_dirty_source_vetoes_qualification_before_collection()
    test_clean_source_returns_complete_without_grading_artifacts()
    test_source_change_after_collection_vetoes_completion()
    test_serial_boundary_waits_for_attach_time_boot_past_initial_observation()
    test_missing_and_malformed_boot_identity_fail()
    test_conflicting_boot_identities_fail()
    test_serial_boundary_fails_if_detected_startup_never_reaches_boot_identity()
    test_native_usb_reset_refuses_other_ports_before_control_changes()
    test_explicit_reset_records_request_before_control_and_never_completes_failure()
    test_explicit_boundary_requires_fresh_usb_reset_and_refuses_intervening_failure()
    test_serial_carriage_return_framing_preserves_reset_evidence_and_failures()
    test_serial_interrupted_loader_framing_requires_one_exact_rom_banner()
    test_presentation_capture_retains_visual_inputs_without_private_profile_names()
    test_presentation_capture_accepts_unassigned_slot_but_rejects_malformed_slot()
    test_presentation_selection_intent_is_explicit()
    test_bench_upload_preserves_littlefs_settings()
    test_bench_joins_maintenance_wifi_without_usb_status_query()
    test_bench_retries_wifi_join_during_http_wait()
    test_wifi_join_uses_shared_deadline()
    test_presentation_http_is_maintenance_only_and_precedes_upload()
    test_display_contract_binds_snapshot_stimulus_scenario_runtime_and_camera()
    print("bench window tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
