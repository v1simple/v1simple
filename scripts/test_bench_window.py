#!/usr/bin/env python3
"""Focused process-lifecycle tests for the unified bench window collector."""

from __future__ import annotations

import io
import json
import os
import signal
import stat
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts" / "bench"))

import camera_capture as camera_capture_module  # noqa: E402
import camera_grade as camera_grade_module  # noqa: E402
import run_logged as run_logged_module  # noqa: E402
import run_window as run_window_module  # noqa: E402
from camera_capture import (  # noqa: E402
    CALIBRATION_PATCH,
    FRAME_BYTES,
    FRAME_HEIGHT,
    FRAME_WIDTH,
    VIDEO_EXPOSURE,
    CameraCapture,
    evaluate_camera_profile_frames,
)
from camera_grade import (  # noqa: E402
    DISPLAY_CROP_HEIGHT,
    DISPLAY_CROP_WIDTH,
    DISPLAY_CROP_X,
    DISPLAY_CROP_Y,
    EncounterObservation,
    FrameObservation,
    MAX_REPLAY_ALIGNMENT_ADJUSTMENT_S,
    REFERENCE_ANCHOR_X,
    REFERENCE_ANCHOR_Y,
    REGISTRATION_HEIGHT,
    REGISTRATION_WIDTH,
    detect_display_crop_registration,
    find_replay_alignment,
    find_replay_offset,
    frequency_signature,
    grade_idle,
    grade_replay,
    identify_frequency,
)
from run_window import (  # noqa: E402
    ALL_VOLUME_REQUEST,
    ALL_VOLUME_RESPONSE,
    EMPTY_ALERT_ROW,
    RECONNECT_LEDGER_NAME,
    RECONNECT_LOG_NAME,
    ReconnectBehaviorError,
    ReconnectPreflightFailure,
    START_ALERT_REQUEST,
    VERSION_REQUEST,
    VERSION_RESPONSE,
    V1Emulator,
    V1_RADIO_LEASE_PATH,
    V1RadioLease,
    _preflight_ledger_is_complete,
    camera_grade_required,
    encounter_csv_sd_path,
    establish_reconnect_readiness,
    establish_serial_fence,
    run_reconnect_preflight,
    start_and_wait,
    wait_for_post_upload_settle,
)


def assert_true(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def write_dummy_emulator(
    path: Path,
    *,
    emit_complete: bool,
    blink_profile: str = "",
    blink_samples: int = 0,
    emit_session_transport: bool = True,
    emit_session_transport_loss: bool = False,
    emit_stopped: bool = True,
    stop_exit_code: int = 0,
    stop_events: tuple[str, ...] | None = None,
) -> None:
    configured = "true"
    if blink_profile:
        source = "generated_multi_alert_assumption" if blink_profile == "scenario" else "explicit_control"
        configured = (
            "echo 'V1REPLAY_EVENT "
            f'{{"state":"configured","blinkProfile":"{blink_profile}",'
            f'"blinkSource":"{source}","blinkSamples":{blink_samples},'
            '"totalSamples":762,"cadenceHz":3}'
            "'"
        )
        configured += "\necho 'status V1REPLAY_EVENT {\"state\":\"replay_started\",\"hostMonotonicSeconds\":12345.5}'"
    marker = 'echo \'V1REPLAY_EVENT {"state":"complete"}\'' if emit_complete else "true"
    session_transport = (
        'echo \'V1REPLAY_EVENT {"state":"session_transport","active":true}\''
        if emit_session_transport
        else "true"
    )
    if emit_session_transport_loss:
        session_transport += (
            '\necho \'V1REPLAY_EVENT {"state":"session_transport","active":false}\''
        )
    if stop_events is None:
        stop_events = (
            'V1REPLAY_EVENT {"state":"stopping","sessionTransportActive":true}',
            'V1REPLAY_EVENT {"state":"session_transport","active":false}',
            *((('V1REPLAY_EVENT {"state":"stopped"}',) if emit_stopped else ())),
        )
    stop_commands = "; ".join(
        'echo "' + event.replace('"', '\\"') + '"' for event in stop_events
    ) or "true"
    path.write_text(
        "#!/bin/sh\n"
        f"trap '{stop_commands}; exit {stop_exit_code}' TERM INT\n"
        "echo argv=$*\n"
        f"{configured}\n"
        f"{marker}\n"
        f"{session_transport}\n"
        "while :; do sleep 1; done\n",
        encoding="utf-8",
    )
    path.chmod(path.stat().st_mode | stat.S_IXUSR)


def wait_for_dummy_emulator_started(emulator: V1Emulator, timeout_s: float = 2) -> None:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        try:
            if "argv=" in emulator.log_path.read_text(encoding="utf-8"):
                return
        except OSError:
            pass
        time.sleep(0.02)
    raise AssertionError("dummy emulator did not reach its signal-ready marker")


def handshake_ledger_records(
    *,
    start_elapsed_ms: tuple[int, ...] = (100,),
    include_stream: bool = True,
) -> list[dict[str, object]]:
    after_starts = start_elapsed_ms[-1] + 50
    events: list[dict[str, object]] = [
        {"event": "subscribe", "epoch": 1, "channel": "B2CE", "elapsed_ms": 0},
        *(
            {
                "event": "request", "epoch": 1, "channel": "B6D4",
                "bytes": START_ALERT_REQUEST, "elapsed_ms": elapsed_ms,
            }
            for elapsed_ms in start_elapsed_ms
        ),
    ]
    if include_stream:
        events.append({
            "event": "stream_started", "epoch": 1, "channel": "B2CE",
            "bytes": EMPTY_ALERT_ROW, "delivery": "delivered",
            "elapsed_ms": after_starts,
        })
    events.extend([
        {
            "event": "request", "epoch": 1, "channel": "B6D4",
            "bytes": VERSION_REQUEST, "elapsed_ms": after_starts + 50,
        },
        {
            "event": "response", "epoch": 1, "channel": "B2CE",
            "bytes": VERSION_RESPONSE, "delivery": "delivered",
            "elapsed_ms": after_starts + 100,
        },
        {
            "event": "request", "epoch": 1, "channel": "B6D4",
            "bytes": ALL_VOLUME_REQUEST, "elapsed_ms": after_starts + 150,
        },
        {
            "event": "response", "epoch": 1, "channel": "B2CE",
            "bytes": ALL_VOLUME_RESPONSE, "delivery": "delivered",
            "elapsed_ms": after_starts + 200,
        },
    ])
    return [
        {
            "schema_version": 2,
            "kind": "v1replay_handshake_ledger",
            "timebase": "epoch_monotonic_ms",
        },
        *events,
    ]


def write_handshake_ledger(
    path: Path,
    *,
    start_elapsed_ms: tuple[int, ...] = (100,),
    include_stream: bool = True,
) -> None:
    records = handshake_ledger_records(
        start_elapsed_ms=start_elapsed_ms,
        include_stream=include_stream,
    )
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        "".join(json.dumps(record, separators=(",", ":")) + "\n" for record in records),
        encoding="utf-8",
    )


def test_idle_emulator_covers_and_stops_with_window() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        executable = root / "v1replay"
        write_dummy_emulator(executable, emit_complete=False)
        emulator = V1Emulator(executable, root / "core", "core")
        emulator.start()
        emulator.wait_for_session_transport(1)
        assert_true(emulator.health_problem() == "", "idle emulator exited before the window")
        result = emulator.finish(window_completed=True)
        assert_true(result["completed"] is True, f"idle emulator did not cover window: {result}")
        assert_true(result["mode"] == "idle", f"wrong core emulator mode: {result}")
        assert_true(result["managed_stop"] is True, f"runner did not own cleanup: {result}")
        assert_true(emulator.process is not None and emulator.process.poll() is not None, "emulator survived cleanup")


def test_failed_window_still_stops_emulator() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        executable = root / "v1replay"
        write_dummy_emulator(executable, emit_complete=False)
        emulator = V1Emulator(executable, root / "core", "core")
        emulator.start()
        emulator.wait_for_session_transport(1)
        result = emulator.finish(window_completed=False)
        assert_true(result["completed"] is False, f"failed collection was marked complete: {result}")
        assert_true(result["managed_stop"] is True, f"failed collection skipped cleanup: {result}")
        assert_true(emulator.process is not None and emulator.process.poll() is not None, "emulator survived failed collection")


def test_idle_emulator_requires_current_process_transport_ownership() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        executable = root / "v1replay"
        write_dummy_emulator(
            executable,
            emit_complete=False,
            emit_session_transport=False,
        )
        emulator = V1Emulator(executable, root / "core", "core")
        emulator.start()
        wait_for_dummy_emulator_started(emulator)
        try:
            emulator.wait_for_session_transport(0.5)
        except RuntimeError as exc:
            assert_true(
                "current-process session transport ownership" in str(exc),
                f"wrong missing-ownership failure: {exc}",
            )
        else:
            raise AssertionError("idle emulator without an ownership event was admitted")
        finally:
            emulator.stop()


def test_managed_stop_requires_graceful_stopped_marker() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        executable = root / "v1replay"
        write_dummy_emulator(
            executable,
            emit_complete=False,
            emit_stopped=False,
        )
        emulator = V1Emulator(executable, root / "core", "core")
        emulator.start()
        emulator.wait_for_session_transport(1)
        try:
            emulator.finish(window_completed=True)
        except RuntimeError as exc:
            assert_true(
                "graceful stopped marker" in str(exc),
                f"wrong ungraceful-stop failure: {exc}",
            )
        else:
            raise AssertionError("managed stop without a stopped marker passed")
        assert_true(
            emulator.process is not None and emulator.process.poll() is not None,
            "missing stopped marker left the emulator alive",
        )


def test_managed_stop_rejects_nonzero_exit_after_stopped_marker() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        executable = root / "v1replay"
        write_dummy_emulator(
            executable,
            emit_complete=False,
            stop_exit_code=7,
        )
        emulator = V1Emulator(executable, root / "core", "core")
        emulator.start()
        emulator.wait_for_session_transport(1)
        try:
            emulator.finish(window_completed=True)
        except RuntimeError as exc:
            assert_true(
                "graceful teardown exited with code 7" in str(exc),
                f"wrong nonzero-stop failure: {exc}",
            )
        else:
            raise AssertionError("a stopped marker hid the emulator's nonzero exit")
        assert_true(
            emulator.returncode == 7 and emulator.graceful_stop_confirmed is False,
            "nonzero exit was retained as graceful evidence",
        )


def test_all_managed_modes_require_current_stopping_ownership_snapshot() -> None:
    cases = (
        ("replay-false", False, False, "did not own session transport"),
        ("replay-nonboolean", False, "true", "snapshot is not boolean"),
        ("preflight-false", True, False, "did not own session transport"),
    )
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        for name, handshake_only, snapshot_value, expected in cases:
            case_root = root / name
            case_root.mkdir()
            executable = case_root / "v1replay"
            encoded_snapshot = json.dumps(snapshot_value, separators=(",", ":"))
            write_dummy_emulator(
                executable,
                emit_complete=True,
                blink_profile="scenario",
                blink_samples=57,
                stop_events=(
                    "V1REPLAY_EVENT "
                    '{"state":"stopping","sessionTransportActive":'
                    f"{encoded_snapshot}}}",
                    'V1REPLAY_EVENT {"state":"session_transport","active":false}',
                    'V1REPLAY_EVENT {"state":"stopped"}',
                ),
            )
            emulator = V1Emulator(
                executable,
                case_root / "out",
                "replay",
                handshake_only=handshake_only,
            )
            emulator.start()
            wait_for_dummy_emulator_started(emulator)
            try:
                if handshake_only:
                    emulator.finish_preflight(handshake_ready_while_alive=True)
                else:
                    emulator.finish(window_completed=True)
            except RuntimeError as exc:
                assert_true(expected in str(exc), f"wrong {name} snapshot failure: {exc}")
            else:
                raise AssertionError(f"managed mode accepted invalid stopping snapshot: {name}")
            assert_true(
                emulator.graceful_stop_confirmed is False,
                f"invalid {name} snapshot was retained as graceful stop evidence",
            )


def test_idle_completion_rejects_transport_loss_even_after_reownership() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        executable = root / "v1replay"
        write_dummy_emulator(
            executable,
            emit_complete=False,
            stop_events=(
                'V1REPLAY_EVENT {"state":"session_transport","active":false}',
                'V1REPLAY_EVENT {"state":"session_transport","active":true}',
                'V1REPLAY_EVENT {"state":"stopping","sessionTransportActive":true}',
                'V1REPLAY_EVENT {"state":"session_transport","active":false}',
                'V1REPLAY_EVENT {"state":"stopped"}',
            ),
        )
        emulator = V1Emulator(executable, root / "core", "core")
        emulator.start()
        emulator.wait_for_session_transport(1)
        try:
            emulator.finish(window_completed=True)
        except RuntimeError as exc:
            assert_true(
                "lost session transport before the stopping boundary" in str(exc),
                f"wrong interrupted-transport failure: {exc}",
            )
        else:
            raise AssertionError("final reownership hid an interrupted idle transport")


def test_idle_completion_grades_loss_during_managed_stop() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        executable = root / "v1replay"
        write_dummy_emulator(
            executable,
            emit_complete=False,
            stop_events=(
                'V1REPLAY_EVENT {"state":"session_transport","active":false}',
                'V1REPLAY_EVENT {"state":"stopping","sessionTransportActive":false}',
                'V1REPLAY_EVENT {"state":"stopped"}',
            ),
        )
        emulator = V1Emulator(executable, root / "core", "core")
        emulator.start()
        emulator.wait_for_session_transport(1)
        try:
            emulator.finish(window_completed=True)
        except RuntimeError as exc:
            assert_true(
                "did not own session transport at the stopping boundary" in str(exc),
                f"wrong stop-race ownership failure: {exc}",
            )
        else:
            raise AssertionError("transport loss between admission and managed stop passed")


def test_idle_shutdown_requires_prior_admission_and_strict_ordered_events() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        executable = root / "unadmitted" / "v1replay"
        executable.parent.mkdir()
        write_dummy_emulator(executable, emit_complete=False)
        unadmitted = V1Emulator(executable, root / "unadmitted" / "out", "core")
        unadmitted.start()
        wait_for_dummy_emulator_started(unadmitted)
        try:
            unadmitted.finish(window_completed=True)
        except RuntimeError as exc:
            assert_true(
                "without prior current-process ownership admission" in str(exc),
                f"wrong missing-admission failure: {exc}",
            )
        else:
            raise AssertionError("a child log event substituted for host ownership admission")

        canonical_stopping = (
            'V1REPLAY_EVENT {"state":"stopping","sessionTransportActive":true}',
            'V1REPLAY_EVENT {"state":"session_transport","active":false}',
            'V1REPLAY_EVENT {"state":"stopped"}',
        )
        mutants = (
            (
                "malformed",
                ('V1REPLAY_EVENT {"state":', *canonical_stopping),
                "malformed V1 emulator machine event",
            ),
            (
                "missing-stopping",
                canonical_stopping[1:],
                "exactly one stopping ownership snapshot",
            ),
            (
                "duplicate-stopping",
                (canonical_stopping[0], *canonical_stopping),
                "exactly one stopping ownership snapshot",
            ),
            (
                "stopping-false",
                (
                    'V1REPLAY_EVENT {"state":"stopping","sessionTransportActive":false}',
                    *canonical_stopping[1:],
                ),
                "did not own session transport at the stopping boundary",
            ),
            (
                "stopping-nonboolean",
                (
                    'V1REPLAY_EVENT {"state":"stopping","sessionTransportActive":"true"}',
                    *canonical_stopping[1:],
                ),
                "stopping ownership snapshot is not boolean",
            ),
            (
                "teardown-missing",
                (canonical_stopping[0], canonical_stopping[2]),
                "without a teardown session transport event",
            ),
            (
                "teardown-nonboolean",
                (
                    canonical_stopping[0],
                    'V1REPLAY_EVENT {"state":"session_transport","active":"false"}',
                    canonical_stopping[2],
                ),
                "session transport event is not boolean",
            ),
            (
                "stopped-before-teardown",
                (
                    canonical_stopping[0],
                    canonical_stopping[2],
                    canonical_stopping[1],
                ),
                "stopped marker is not the final machine event",
            ),
            (
                "event-after-stopped",
                (*canonical_stopping, 'V1REPLAY_EVENT {"state":"configured"}'),
                "stopped marker is not the final machine event",
            ),
        )
        for name, stop_events, expected in mutants:
            case_root = root / name
            case_root.mkdir()
            case_executable = case_root / "v1replay"
            write_dummy_emulator(
                case_executable,
                emit_complete=False,
                stop_events=stop_events,
            )
            emulator = V1Emulator(case_executable, case_root / "out", "core")
            emulator.start()
            emulator.wait_for_session_transport(1)
            try:
                emulator.finish(window_completed=True)
            except RuntimeError as exc:
                assert_true(expected in str(exc), f"wrong {name} failure: {exc}")
            else:
                raise AssertionError(f"idle shutdown mutant passed: {name}")


def test_idle_admission_rejects_transport_already_lost() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        executable = root / "v1replay"
        write_dummy_emulator(
            executable,
            emit_complete=False,
            emit_session_transport_loss=True,
        )
        emulator = V1Emulator(executable, root / "core", "core")
        emulator.start()
        wait_for_dummy_emulator_started(emulator)
        try:
            emulator.wait_for_session_transport(0.5)
        except RuntimeError as exc:
            assert_true(
                "current-process session transport ownership" in str(exc),
                f"wrong stale-ownership failure: {exc}",
            )
        else:
            raise AssertionError("historical transport ownership admitted a disconnected peer")
        finally:
            emulator.stop()


def test_qstart_companion_failures_abort_the_active_dut_window() -> None:
    class FakeSerial:
        def __init__(self) -> None:
            self.commands: list[str] = []

        def write_command(self, command: str) -> None:
            self.commands.append(command)

        def read_protocol_line(
            self,
            _prefixes: tuple[str, ...],
            _timeout: float,
        ) -> str:
            if self.commands[-1] == "QABORT":
                return (
                    'QRESP {"ok":false,"state":"error","suite":"core",'
                    '"message":"aborted","error":"aborted"}'
                )
            return (
                'QRESP {"ok":true,"state":"running","suite":"core",'
                '"csvPath":"/perf/test.csv"}'
            )

    def fail_admission() -> None:
        raise RuntimeError("idle transport ownership missing")

    class LostAckThenBusySerial(FakeSerial):
        def read_protocol_line(
            self,
            _prefixes: tuple[str, ...],
            _timeout: float,
        ) -> str:
            if self.commands[-1] == "QABORT":
                return super().read_protocol_line(_prefixes, _timeout)
            starts = sum(command.startswith("QSTART ") for command in self.commands)
            if starts == 1:
                raise TimeoutError("running acknowledgement was lost")
            return 'QERR {"ok":false,"state":"running","error":"busy"}'

    ambiguous_start_serial = LostAckThenBusySerial()
    try:
        start_and_wait(
            ambiguous_start_serial,  # type: ignore[arg-type]
            "core",
            1,
            1,
        )
    except RuntimeError as exc:
        assert_true("QSTART failed" in str(exc), f"wrong ambiguous-start failure: {exc}")
        assert_true(
            "QABORT cleanup was not confirmed" not in str(exc),
            f"ambiguous start did not consume its abort acknowledgement: {exc}",
        )
    else:
        raise AssertionError("lost QSTART acknowledgement followed by busy passed")
    assert_true(
        ambiguous_start_serial.commands
        == ["QSTART core 1", "QSTART core 1", "QABORT"],
        f"ambiguous QSTART left the DUT window active: {ambiguous_start_serial.commands}",
    )

    class MalformedStartSerial(FakeSerial):
        def __init__(self, response: str) -> None:
            super().__init__()
            self.response = response

        def read_protocol_line(
            self,
            _prefixes: tuple[str, ...],
            _timeout: float,
        ) -> str:
            if self.commands[-1] == "QABORT":
                return super().read_protocol_line(_prefixes, _timeout)
            return self.response

    for response, expected in (
        ("QRESP []", "JSON object"),
        (
            'QRESP {"ok":"false","state":"running","suite":"core"}',
            "ok field is not boolean",
        ),
    ):
        malformed_serial = MalformedStartSerial(response)
        try:
            start_and_wait(
                malformed_serial,  # type: ignore[arg-type]
                "core",
                1,
                1,
            )
        except RuntimeError as exc:
            assert_true(expected in str(exc), f"wrong malformed-ack failure: {exc}")
        else:
            raise AssertionError(f"malformed QSTART acknowledgement passed: {response}")
        assert_true(
            malformed_serial.commands == ["QSTART core 1", "QABORT"],
            f"malformed QSTART left the DUT window active: {malformed_serial.commands}",
        )

    after_started_serial = FakeSerial()
    try:
        start_and_wait(
            after_started_serial,  # type: ignore[arg-type]
            "core",
            1,
            1,
            after_started=fail_admission,
        )
    except RuntimeError as exc:
        assert_true("ownership missing" in str(exc), f"wrong admission failure: {exc}")
    else:
        raise AssertionError("companion admission failure did not escape")
    assert_true(
        after_started_serial.commands == ["QSTART core 1", "QABORT"],
        f"admission failure left the DUT window active: {after_started_serial.commands}",
    )

    health_serial = FakeSerial()

    def fail_health() -> str:
        raise RuntimeError("companion health crashed")

    try:
        start_and_wait(
            health_serial,  # type: ignore[arg-type]
            "core",
            1,
            1,
            health_check=fail_health,
        )
    except RuntimeError as exc:
        assert_true("health crashed" in str(exc), f"wrong health failure: {exc}")
    else:
        raise AssertionError("health-check exception did not escape")
    assert_true(
        health_serial.commands == ["QSTART core 1", "QABORT"],
        f"health failure left the DUT window active: {health_serial.commands}",
    )

    class StaleThenConfirmedAbortSerial(FakeSerial):
        def __init__(self) -> None:
            super().__init__()
            self.abort_reads = 0

        def read_protocol_line(
            self,
            _prefixes: tuple[str, ...],
            _timeout: float,
        ) -> str:
            if self.commands[-1] != "QABORT":
                return super().read_protocol_line(_prefixes, _timeout)
            self.abort_reads += 1
            if self.abort_reads == 1:
                return 'QERR {"ok":false,"state":"running","error":"run_active"}'
            return super().read_protocol_line(_prefixes, _timeout)

    stale_serial = StaleThenConfirmedAbortSerial()
    try:
        start_and_wait(
            stale_serial,  # type: ignore[arg-type]
            "core",
            1,
            1,
            after_started=fail_admission,
        )
    except RuntimeError as exc:
        assert_true("ownership missing" in str(exc), f"wrong stale-response failure: {exc}")
        assert_true(
            "QABORT cleanup was not confirmed" not in str(exc),
            f"stale response hid the later abort acknowledgement: {exc}",
        )
    else:
        raise AssertionError("companion failure did not escape after confirmed abort")
    assert_true(stale_serial.abort_reads == 2, "abort confirmation ignored a stale response")

    class InterruptedReadSerial(FakeSerial):
        def __init__(self) -> None:
            super().__init__()
            self.reads = 0

        def read_protocol_line(
            self,
            _prefixes: tuple[str, ...],
            _timeout: float,
        ) -> str:
            if self.commands[-1] == "QABORT":
                return super().read_protocol_line(_prefixes, _timeout)
            self.reads += 1
            if self.reads == 1:
                return super().read_protocol_line(_prefixes, _timeout)
            raise InterruptedError("operator interrupted the serial wait")

    interrupted_serial = InterruptedReadSerial()
    try:
        start_and_wait(
            interrupted_serial,  # type: ignore[arg-type]
            "core",
            1,
            1,
        )
    except InterruptedError as exc:
        assert_true("operator interrupted" in str(exc), f"wrong interruption: {exc}")
    else:
        raise AssertionError("serial-wait interruption did not escape")
    assert_true(
        interrupted_serial.commands == ["QSTART core 1", "QABORT"],
        f"serial interruption left the DUT window active: {interrupted_serial.commands}",
    )

    class TerminalMutationSerial(FakeSerial):
        def __init__(self, terminal: str) -> None:
            super().__init__()
            self.terminal = terminal
            self.reads = 0

        def read_protocol_line(
            self,
            _prefixes: tuple[str, ...],
            _timeout: float,
        ) -> str:
            if self.commands[-1] == "QABORT":
                return super().read_protocol_line(_prefixes, _timeout)
            self.reads += 1
            if self.reads == 1:
                return super().read_protocol_line(_prefixes, _timeout)
            return self.terminal

    for terminal, expected in (
        (
            'QEVENT {"ok":"false","state":"done","suite":"core"}',
            "terminal event ok field is not boolean",
        ),
        (
            'QEVENT {"ok":true,"state":"done","suite":"display"}',
            "suite='display' expected='core'",
        ),
        (
            'QERR {"ok":true,"state":"done","suite":"core"}',
            "terminal response was not QEVENT",
        ),
    ):
        terminal_serial = TerminalMutationSerial(terminal)
        try:
            start_and_wait(
                terminal_serial,  # type: ignore[arg-type]
                "core",
                1,
                1,
            )
        except RuntimeError as exc:
            assert_true(expected in str(exc), f"wrong terminal-shape failure: {exc}")
        else:
            raise AssertionError(f"invalid terminal event passed: {terminal}")
        assert_true(
            terminal_serial.commands == ["QSTART core 1", "QABORT"],
            f"invalid terminal event left the DUT window active: {terminal_serial.commands}",
        )

    timeout_serial = FakeSerial()
    try:
        start_and_wait(
            timeout_serial,  # type: ignore[arg-type]
            "core",
            0,
            0,
        )
    except RuntimeError as exc:
        assert_true("timed out waiting for completion" in str(exc), f"wrong timeout: {exc}")
    else:
        raise AssertionError("window timeout did not escape")
    assert_true(
        timeout_serial.commands == ["QSTART core 0", "QABORT"],
        f"window timeout left the DUT window active: {timeout_serial.commands}",
    )

    class UnconfirmedAbortSerial(FakeSerial):
        def __init__(self) -> None:
            super().__init__()
            self.abort_reads = 0

        def read_protocol_line(
            self,
            _prefixes: tuple[str, ...],
            _timeout: float,
        ) -> str:
            if self.commands[-1] != "QABORT":
                return super().read_protocol_line(_prefixes, _timeout)
            self.abort_reads += 1
            if self.abort_reads == 1:
                return 'QERR {"ok":false,"state":"running","error":"run_active"}'
            raise TimeoutError("abort acknowledgement timed out")

    unconfirmed_serial = UnconfirmedAbortSerial()
    try:
        start_and_wait(
            unconfirmed_serial,  # type: ignore[arg-type]
            "core",
            1,
            1,
            after_started=fail_admission,
        )
    except RuntimeError as exc:
        assert_true(
            "ownership missing" in str(exc)
            and "QABORT cleanup was not confirmed" in str(exc),
            f"unconfirmed abort hid its companion failure: {exc}",
        )
    else:
        raise AssertionError("an unconfirmed QABORT was accepted")


def test_radio_lease_is_inherited_and_owner_pid_is_forwarded() -> None:
    assert_true(
        ROOT not in V1_RADIO_LEASE_PATH.parents,
        f"radio lease is vulnerable to repository artifact cleanup: {V1_RADIO_LEASE_PATH}",
    )
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        executable = root / "v1replay"
        out_dir = root / "core"
        lock_path = root / "v1radio.lock"
        write_dummy_emulator(executable, emit_complete=False)
        lease = V1RadioLease(lock_path, quiet_seconds=0)
        lease.__enter__()
        assert lease.fd is not None
        emulator = V1Emulator(
            executable,
            out_dir,
            "core",
            lease_fd=lease.fd,
        )
        emulator.start()
        emulator.wait_for_session_transport(1)
        lease.close()
        try:
            with V1RadioLease(lock_path, quiet_seconds=0):
                pass
        except RuntimeError as exc:
            assert_true("radio lease unavailable" in str(exc), f"wrong lease failure: {exc}")
        else:
            raise AssertionError("parent close released a lease still inherited by the child")

        result = emulator.finish(window_completed=True)
        assert_true(result["graceful_stop_confirmed"] is True, f"stop was not proven: {result}")
        with V1RadioLease(lock_path, quiet_seconds=0):
            pass
        log = (out_dir / "v1replay.log").read_text(encoding="utf-8")
        assert_true(
            f"argv=idle --machine-events --owner-pid {os.getpid()}" in log,
            f"owner PID was not forwarded to the child: {log!r}",
        )


def test_first_signal_makes_cleanup_non_interruptible() -> None:
    original_signal = run_window_module.signal.signal
    registrations: list[tuple[int, object]] = []

    def record_signal(signum: int, handler: object) -> None:
        registrations.append((signum, handler))

    run_window_module.signal.signal = record_signal  # type: ignore[assignment]
    try:
        run_window_module.install_signal_handlers()
        handler = next(
            handler
            for signum, handler in registrations
            if signum == signal.SIGTERM and callable(handler)
        )
        try:
            handler(signal.SIGTERM, None)  # type: ignore[operator]
        except InterruptedError:
            pass
        else:
            raise AssertionError("first termination signal did not begin cleanup")
        ignored = registrations[-3:]
        assert_true(
            {signum for signum, handler_value in ignored if handler_value == signal.SIG_IGN}
            == {signal.SIGINT, signal.SIGTERM, signal.SIGHUP},
            f"cleanup signals were not ignored after the first signal: {ignored}",
        )
        handler(signal.SIGTERM, None)  # type: ignore[operator]
    finally:
        run_window_module.signal.signal = original_signal  # type: ignore[assignment]


def test_live_cleanup_stops_emulators_before_serial_and_camera() -> None:
    source = (ROOT / "scripts" / "bench" / "run_window.py").read_text(encoding="utf-8")
    cleanup = source.index("finally:\n        primary_error = sys.exc_info()[1]")
    primary_stop = source.index("emulator.finish(collection_completed)", cleanup)
    preflight_stop = source.index("reconnect_preflight.stop()", primary_stop)
    serial_stop = source.index("q.close()", preflight_stop)
    camera_stop = source.index("camera.stop(collection_completed)", serial_stop)
    assert_true(
        primary_stop < preflight_stop < serial_stop < camera_stop,
        "live cleanup does not withdraw both emulators before serial and camera teardown",
    )


def test_live_cleanup_preserves_primary_failure_when_emulator_stop_also_fails() -> None:
    serials: list[object] = []
    cleanup_failure = ""
    serial_cleanup_failure = ""

    class FakeSerial:
        boot_marker_count = 0
        disconnect_cleanup_count = 0

        def __init__(self, *_args: object, **_kwargs: object) -> None:
            self.commands: list[str] = []
            self.closed = False
            serials.append(self)

        def write_command(self, command: str) -> None:
            self.commands.append(command)

        def read_protocol_line(
            self,
            _prefixes: tuple[str, ...],
            _timeout: float,
        ) -> str:
            if self.commands[-1] == "QABORT":
                return (
                    'QRESP {"ok":false,"state":"error","suite":"core",'
                    '"message":"aborted","error":"aborted"}'
                )
            return (
                'QRESP {"ok":true,"state":"running","suite":"core",'
                '"csvPath":"/perf/test.csv"}'
            )

        def close(self) -> None:
            self.closed = True
            if serial_cleanup_failure:
                raise RuntimeError(serial_cleanup_failure)

    class FakeEmulator:
        process = None

        def __init__(self, *_args: object, **_kwargs: object) -> None:
            pass

        def start(self) -> None:
            pass

        def wait_for_session_transport(self, _timeout_s: float) -> None:
            raise RuntimeError("idle ownership primary failure")

        def health_problem(self) -> str:
            return ""

        def finish(self, _completed: bool) -> dict[str, object]:
            raise RuntimeError(cleanup_failure)

    originals = (
        run_window_module.wait_for_port,
        run_window_module.BenchSerial,
        run_window_module.wait_ready,
        run_window_module.V1Emulator,
    )
    try:
        run_window_module.wait_for_port = lambda *_args, **_kwargs: "fake-port"
        run_window_module.BenchSerial = FakeSerial
        run_window_module.wait_ready = lambda *_args, **_kwargs: {"ok": True}
        run_window_module.V1Emulator = FakeEmulator
        for cleanup_failure, serial_cleanup_failure in (
            ("managed V1 emulator exited without a graceful stopped marker", ""),
            (
                "managed V1 emulator graceful teardown exited with code 9",
                "serial close failed",
            ),
        ):
            serials.clear()
            with tempfile.TemporaryDirectory() as tmp:
                args = SimpleNamespace(
                    suite="core",
                    camera=False,
                    upload=False,
                    port="fake-port",
                    baud=115200,
                    replay_executable=str(Path(tmp) / "v1replay"),
                    blink_profile="steady",
                    ready_timeout_seconds=1,
                    duration_seconds=1,
                    completion_grace_seconds=1,
                )
                try:
                    run_window_module._collect_live(
                        args,
                        Path(tmp) / "core",
                        lease_fd=73,
                    )
                except RuntimeError as exc:
                    message = str(exc)
                    assert_true(
                        message.startswith("idle ownership primary failure"),
                        f"cleanup masked the primary failure: {message}",
                    )
                    assert_true(
                        f"cleanup failure: V1 emulator: {cleanup_failure}" in message,
                        f"cleanup failure disappeared: {message}",
                    )
                    if serial_cleanup_failure:
                        assert_true(
                            f"; serial: {serial_cleanup_failure}" in message,
                            f"later serial cleanup failure disappeared or reordered: {message}",
                        )
                else:
                    raise AssertionError("simultaneous primary and cleanup failures passed")
            assert_true(len(serials) == 1, f"unexpected serial sessions: {serials}")
            serial = serials[0]
            assert_true(
                getattr(serial, "commands") == ["QSTART core 1", "QABORT"],
                f"primary failure did not abort the DUT window: {getattr(serial, 'commands')}",
            )
            assert_true(getattr(serial, "closed") is True, "serial cleanup did not run")
    finally:
        (
            run_window_module.wait_for_port,
            run_window_module.BenchSerial,
            run_window_module.wait_ready,
            run_window_module.V1Emulator,
        ) = originals


def test_replay_requires_machine_completion_before_managed_stop() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        executable = root / "v1replay"
        write_dummy_emulator(
            executable,
            emit_complete=True,
            blink_profile="scenario",
            blink_samples=57,
        )
        emulator = V1Emulator(executable, root / "replay", "replay")
        emulator.start()
        deadline = time.monotonic() + 1
        while time.monotonic() < deadline and not emulator._bench_completed():
            time.sleep(0.02)
        result = emulator.finish(window_completed=True)
        assert_true(result["completed"] is True, f"completion marker was not honored: {result}")
        assert_true(result["mode"] == "bench", f"wrong replay mode: {result}")
        assert_true(
            result["replay_started_monotonic_seconds"] == 12345.5,
            f"first replay sample time was not recorded: {result}",
        )

        missing_root = root / "missing"
        missing_executable = missing_root / "v1replay"
        missing_root.mkdir()
        write_dummy_emulator(
            missing_executable,
            emit_complete=False,
            blink_profile="scenario",
            blink_samples=57,
        )
        missing = V1Emulator(missing_executable, missing_root / "out", "replay")
        missing.start()
        wait_for_dummy_emulator_started(missing)
        missing_result = missing.finish(window_completed=True)
        assert_true(missing_result["completed"] is False, f"incomplete replay passed: {missing_result}")

        unconfigured_root = root / "unconfigured"
        unconfigured_executable = unconfigured_root / "v1replay"
        unconfigured_root.mkdir()
        write_dummy_emulator(unconfigured_executable, emit_complete=True)
        unconfigured = V1Emulator(unconfigured_executable, unconfigured_root / "out", "replay")
        unconfigured.start()
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline and not unconfigured._bench_completed():
            time.sleep(0.02)
        unconfigured_result = unconfigured.finish(window_completed=True)
        assert_true(
            unconfigured_result["completed"] is False,
            f"replay without blink provenance passed: {unconfigured_result}",
        )


def test_replay_blink_profile_argv_and_result() -> None:
    for blink_profile, blink_samples in (
        ("scenario", 57),
        ("steady", 0),
        ("stress", 708),
    ):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            executable = root / "v1replay"
            out_dir = root / "replay"
            write_dummy_emulator(
                executable,
                emit_complete=True,
                blink_profile=blink_profile,
                blink_samples=blink_samples,
            )
            emulator = V1Emulator(executable, out_dir, "replay", blink_profile)
            emulator.start()
            deadline = time.monotonic() + 1
            while time.monotonic() < deadline and not emulator._bench_completed():
                time.sleep(0.02)
            result = emulator.finish(window_completed=True)
            log = (out_dir / "v1replay.log").read_text(encoding="utf-8")
            expected_ledger = out_dir / "handshake_ledger.jsonl"
            expected_argv = (
                f"argv=bench --machine-events --owner-pid {os.getpid()} "
                f"--blink-profile {blink_profile} "
                f"--handshake-ledger {expected_ledger}"
            )
            assert_true(expected_argv in log, f"unexpected replay argv: {log!r}")
            assert_true(
                result["handshake_ledger"] == expected_ledger.name,
                f"handshake ledger provenance was not recorded: {result}",
            )
            assert_true(
                result["blink_profile"] == blink_profile,
                f"blink profile provenance was not recorded: {result}",
            )
            assert_true(result["blink_samples"] == blink_samples, f"wrong blink exposure: {result}")
            assert_true(
                result["blink_nominal_seconds"] == blink_samples / 3,
                f"wrong nominal blink duration: {result}",
            )


def test_reconnect_preflight_ledger_requires_one_bounded_epoch() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / RECONNECT_LEDGER_NAME
        write_handshake_ledger(path)
        assert_true(_preflight_ledger_is_complete(path), "canonical preflight ledger was not ready")

        records = [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()]
        records[-1]["epoch"] = 2
        path.write_text(
            "".join(json.dumps(record, separators=(",", ":")) + "\n" for record in records),
            encoding="utf-8",
        )
        try:
            _preflight_ledger_is_complete(path)
        except ReconnectBehaviorError as exc:
            assert_true(
                exc.kind == "handshake_invalid" and "one anonymous epoch" in str(exc),
                f"wrong extra-epoch failure: {exc}",
            )
        else:
            raise AssertionError("cross-epoch preflight evidence passed")

        records = handshake_ledger_records()
        second_epoch = json.loads(json.dumps(records[1:]))
        for event in second_epoch:
            event["epoch"] = 2
        path.write_text(
            "".join(
                json.dumps(record, separators=(",", ":")) + "\n"
                for record in [*records, *second_epoch]
            ),
            encoding="utf-8",
        )
        try:
            _preflight_ledger_is_complete(path)
        except ReconnectBehaviorError as exc:
            assert_true(
                exc.kind == "handshake_invalid" and "one anonymous epoch" in str(exc),
                f"wrong full second-epoch failure: {exc}",
            )
        else:
            raise AssertionError("two complete ledger epochs passed")

        write_handshake_ledger(path)
        complete = path.read_text(encoding="utf-8")
        path.write_text(complete[:-1], encoding="utf-8")
        assert_true(
            not _preflight_ledger_is_complete(path),
            "concurrently written partial final line was treated as malformed or complete",
        )


def test_reconnect_preflight_ledger_accepts_only_bounded_timed_pre_stream_retries() -> None:
    def write_records(path: Path, records: list[dict[str, object]]) -> None:
        path.write_text(
            "".join(json.dumps(record, separators=(",", ":")) + "\n" for record in records),
            encoding="utf-8",
        )

    def expect_invalid(path: Path, expected: str) -> None:
        try:
            _preflight_ledger_is_complete(path)
        except ReconnectBehaviorError as exc:
            assert_true(
                exc.kind == "handshake_invalid" and expected in str(exc),
                f"wrong retry failure: {exc}",
            )
        else:
            raise AssertionError(f"invalid retry ledger passed: {expected}")

    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / RECONNECT_LEDGER_NAME

        write_handshake_ledger(path, start_elapsed_ms=(100, 1100))
        assert_true(_preflight_ledger_is_complete(path), "1000 ms retry did not pass")

        write_handshake_ledger(path, start_elapsed_ms=(100, 1101))
        assert_true(_preflight_ledger_is_complete(path), "1001 ms retry did not pass")

        for starts in (
            (100, 1100, 2100),
            (100, 1100, 2100, 3100),
        ):
            write_handshake_ledger(path, start_elapsed_ms=starts)
            assert_true(
                _preflight_ledger_is_complete(path),
                f"valid {len(starts)}-start ledger did not pass",
            )

        write_handshake_ledger(path, start_elapsed_ms=(100, 1099))
        expect_invalid(path, "before the 1000 ms recovery interval")

        write_handshake_ledger(path, start_elapsed_ms=(100, 1100, 2099))
        expect_invalid(path, "before the 1000 ms recovery interval")

        five_starts = (100, 1100, 2100, 3100, 4100)
        write_handshake_ledger(path, start_elapsed_ms=five_starts)
        assert_true(_preflight_ledger_is_complete(path), "bounded fifth start did not pass")

        write_handshake_ledger(path, start_elapsed_ms=(*five_starts, 5100))
        expect_invalid(path, "exceeds its bounded pre-stream start retries")

        switched = handshake_ledger_records(start_elapsed_ms=(100, 1100))
        switched[2]["channel"] = "BAD4"
        write_records(path, switched)
        expect_invalid(path, "switches its selected command channel")

        records = handshake_ledger_records()
        post_stream = dict(records[2])
        post_stream["elapsed_ms"] = 1150
        records.insert(4, post_stream)
        for event in records[5:]:
            event["elapsed_ms"] = max(int(event["elapsed_ms"]), 1200)
        write_records(path, records)
        expect_invalid(path, "after stream delivery")

        incomplete = handshake_ledger_records(
            start_elapsed_ms=(100, 1100),
            include_stream=False,
        )
        write_records(path, incomplete)
        assert_true(
            not _preflight_ledger_is_complete(path),
            "valid retry evidence without delivery was terminalized early",
        )
        incomplete.append({
            "event": "stream_started", "epoch": 1, "channel": "B2CE",
            "bytes": EMPTY_ALERT_ROW, "delivery": "delivered", "elapsed_ms": 1350,
        })
        write_records(path, incomplete)
        assert_true(
            _preflight_ledger_is_complete(path),
            "appended delivery did not complete a valid retry epoch",
        )


def test_reconnect_preflight_ledger_rejects_unverifiable_timing() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / RECONNECT_LEDGER_NAME
        base = handshake_ledger_records(start_elapsed_ms=(100, 1100))

        malformed_values: tuple[object, ...] = (True, 1.5, -1, 0x1_0000_0000)
        for value in malformed_values:
            records = json.loads(json.dumps(base))
            records[2]["elapsed_ms"] = value
            path.write_text(
                "".join(json.dumps(record, separators=(",", ":")) + "\n" for record in records),
                encoding="utf-8",
            )
            try:
                _preflight_ledger_is_complete(path)
            except RuntimeError as exc:
                assert_true("relative timing" in str(exc), f"wrong timing failure: {exc}")
            else:
                raise AssertionError(f"malformed elapsed_ms passed: {value!r}")

        records = json.loads(json.dumps(base))
        records[2].pop("elapsed_ms")
        path.write_text(
            "".join(
                json.dumps(record, separators=(",", ":")) + "\n"
                for record in records
            ),
            encoding="utf-8",
        )
        try:
            _preflight_ledger_is_complete(path)
        except RuntimeError as exc:
            assert_true("event schema" in str(exc), f"wrong missing-time failure: {exc}")
        else:
            raise AssertionError("missing elapsed_ms passed")

        records = json.loads(json.dumps(base))
        records[3]["elapsed_ms"] = 50
        path.write_text(
            "".join(json.dumps(record, separators=(",", ":")) + "\n" for record in records),
            encoding="utf-8",
        )
        try:
            _preflight_ledger_is_complete(path)
        except RuntimeError as exc:
            assert_true("not monotonic" in str(exc), f"wrong clock failure: {exc}")
        else:
            raise AssertionError("decreasing relative timing passed")

        records = json.loads(json.dumps(base))
        records[1]["elapsed_ms"] = 1
        path.write_text(
            "".join(
                json.dumps(record, separators=(",", ":")) + "\n"
                for record in records
            ),
            encoding="utf-8",
        )
        try:
            _preflight_ledger_is_complete(path)
        except RuntimeError as exc:
            assert_true("begin at zero" in str(exc), f"wrong origin failure: {exc}")
        else:
            raise AssertionError("nonzero subscription timing passed")

        legacy = json.loads(json.dumps(base))
        legacy[0] = {"schema_version": 1, "kind": "v1replay_handshake_ledger"}
        for event in legacy[1:]:
            event.pop("elapsed_ms")
        path.write_text(
            "".join(json.dumps(record, separators=(",", ":")) + "\n" for record in legacy),
            encoding="utf-8",
        )
        try:
            _preflight_ledger_is_complete(path)
        except RuntimeError as exc:
            assert_true("invalid header" in str(exc), f"wrong legacy failure: {exc}")
        else:
            raise AssertionError("live schema-1 ledger passed without retry timing")


def test_reconnect_preflight_readiness_uses_delivery_ledger_not_console_order() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        emulator = V1Emulator(root / "unused", root, "replay", handshake_only=True)
        assert emulator.handshake_ledger_path is not None
        write_handshake_ledger(emulator.handshake_ledger_path)
        emulator.health_problem = lambda: ""  # type: ignore[method-assign]
        emulator._bench_event = lambda _state: (_ for _ in ()).throw(  # type: ignore[method-assign]
            AssertionError("console readiness was consulted despite a complete delivery ledger")
        )
        emulator.wait_for_handshake_ready(0.1)


def test_reconnect_preflight_notification_hold_defaults_off_and_forwards_when_selected() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        executable = root / "v1replay"
        write_dummy_emulator(
            executable,
            emit_complete=False,
            blink_profile="scenario",
            blink_samples=57,
        )

        for label, hold_ms, expected_fragment in (
            ("default", 0, ""),
            ("held", 1250, "--handshake-notification-hold-ms 1250"),
        ):
            out_dir = root / label
            emulator = V1Emulator(
                executable,
                out_dir,
                "replay",
                "scenario",
                handshake_only=True,
                handshake_notification_hold_ms=hold_ms,
            )
            try:
                emulator.start()
                deadline = time.monotonic() + 1
                log = ""
                while time.monotonic() < deadline:
                    log = (out_dir / RECONNECT_LOG_NAME).read_text(encoding="utf-8")
                    if "argv=" in log:
                        break
                    time.sleep(0.02)
                assert_true("argv=" in log, f"preflight argv was not recorded: {log!r}")
                if expected_fragment:
                    assert_true(
                        expected_fragment in log,
                        f"notification hold was not forwarded: {log!r}",
                    )
                else:
                    assert_true(
                        "--handshake-notification-hold-ms" not in log,
                        f"default notification hold changed the emulator argv: {log!r}",
                    )
            finally:
                emulator.stop()


def test_reconnect_preflight_notification_hold_rejects_invalid_arguments() -> None:
    root = Path("unused")
    for invalid in (-1, 2_000, True, 1.5):
        try:
            V1Emulator(
                root / "v1replay",
                root / "out",
                "replay",
                handshake_only=True,
                handshake_notification_hold_ms=invalid,  # type: ignore[arg-type]
            )
        except ValueError as exc:
            assert_true("0 through 1999" in str(exc), f"wrong hold error: {exc}")
        else:
            raise AssertionError(f"invalid notification hold passed: {invalid!r}")

    for suite, handshake_only in (("replay", False), ("core", True)):
        try:
            V1Emulator(
                root / "v1replay",
                root / "out",
                suite,
                handshake_only=handshake_only,
                handshake_notification_hold_ms=1,
            )
        except ValueError as exc:
            assert_true("handshake-only" in str(exc), f"wrong mode error: {exc}")
        else:
            raise AssertionError("notification hold passed outside replay handshake-only mode")


def test_reconnect_preflight_observation_catches_late_invalid_ledger() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        ledger_path = Path(tmp) / RECONNECT_LEDGER_NAME
        write_handshake_ledger(ledger_path)

        class FakeSerial:
            boot_marker_count = 0
            disconnect_cleanup_count = 0

            def write_command(self, _command: str) -> None:
                pass

            def read_protocol_line(
                self,
                _prefixes: tuple[str, ...],
                _timeout: float,
            ) -> str:
                return 'QRESP {"ok":true,"state":"idle","suite":"core","mode":"current"}'

            def record_host_boundary(self, _label: str) -> None:
                pass

        class FakeEmulator:
            process = object()
            handshake_ledger_path = ledger_path

            def __init__(self) -> None:
                self.health_checks = 0

            def start(self) -> None:
                pass

            def wait_for_handshake_ready(self, _timeout: float) -> None:
                assert_true(
                    _preflight_ledger_is_complete(self.handshake_ledger_path),
                    "fixture did not become ready",
                )

            def health_problem(self) -> str:
                self.health_checks += 1
                if self.health_checks == 2:
                    records = handshake_ledger_records()
                    late_start = dict(records[2])
                    late_start["elapsed_ms"] = int(records[-1]["elapsed_ms"]) + 50
                    records.append(late_start)
                    self.handshake_ledger_path.write_text(
                        "".join(
                            json.dumps(record, separators=(",", ":")) + "\n"
                            for record in records
                        ),
                        encoding="utf-8",
                    )
                return ""

            def finish_preflight(
                self,
                handshake_ready_while_alive: bool,
            ) -> dict[str, object]:
                return {
                    "handshake_ready_while_alive": handshake_ready_while_alive,
                    "managed_stop": True,
                    "confirmed_exit": True,
                }

        emulator = FakeEmulator()
        try:
            run_reconnect_preflight(
                FakeSerial(),  # type: ignore[arg-type]
                emulator,  # type: ignore[arg-type]
                0.1,
                post_ready_observation_s=0.06,
            )
        except ReconnectPreflightFailure as exc:
            assert_true(
                exc.classification == "FAIL",
                f"late invalid ledger was inconclusive: {exc}",
            )
            assert_true(exc.failure_kind == "handshake_invalid", f"wrong late failure: {exc}")
            assert_true(emulator.health_checks >= 2, "observation did not poll process health")
        else:
            raise AssertionError("late post-readiness ledger violation passed")


def test_reconnect_preflight_observation_rejects_invalid_arguments_before_start() -> None:
    class NeverStartEmulator:
        def __init__(self) -> None:
            self.started = False

        def start(self) -> None:
            self.started = True

    for invalid in (-0.1, True, float("nan"), float("inf"), "1"):
        emulator = NeverStartEmulator()
        try:
            run_reconnect_preflight(
                SimpleNamespace(boot_marker_count=0, disconnect_cleanup_count=0),
                emulator,  # type: ignore[arg-type]
                1,
                post_ready_observation_s=invalid,  # type: ignore[arg-type]
            )
        except ValueError as exc:
            assert_true("finite non-negative" in str(exc), f"wrong observation error: {exc}")
        else:
            raise AssertionError(f"invalid observation duration passed: {invalid!r}")
        assert_true(not emulator.started, "invalid observation duration started the emulator")


def test_reconnect_preflight_pre_stop_fence_timeout_is_narrow_and_evidence_owned() -> None:
    class FakeSerial:
        boot_marker_count = 0
        disconnect_cleanup_count = 0

        def __init__(self, *, timeout_first_fence: bool) -> None:
            self.timeout_first_fence = timeout_first_fence
            self.protocol_timeouts: list[float] = []

        def write_command(self, _command: str) -> None:
            pass

        def read_protocol_line(
            self,
            _prefixes: tuple[str, ...],
            timeout_s: float,
        ) -> str:
            self.protocol_timeouts.append(timeout_s)
            if self.timeout_first_fence and len(self.protocol_timeouts) == 1:
                raise TimeoutError("stress pre-stop fence timed out")
            return 'QRESP {"ok":true,"state":"idle","suite":"core","mode":"current"}'

        def read_line(self, _timeout: float) -> str:
            self.disconnect_cleanup_count += 1
            return "[BLE] V1 disconnected; cleared LCD BLE state at 123 ms"

        def record_host_boundary(self, _label: str) -> None:
            pass

    class FakeEmulator:
        process = object()

        def __init__(self) -> None:
            self.finish_calls = 0

        def start(self) -> None:
            pass

        def wait_for_handshake_ready(self, _timeout: float) -> None:
            pass

        def health_problem(self) -> str:
            return ""

        def _bench_event(self, _state: str) -> dict[str, object]:
            return {"active": True}

        def finish_preflight(
            self,
            handshake_ready_while_alive: bool,
        ) -> dict[str, object]:
            self.finish_calls += 1
            return {
                "handshake_ready_while_alive": handshake_ready_while_alive,
                "managed_stop": True,
                "confirmed_exit": True,
            }

    successful_serial = FakeSerial(timeout_first_fence=False)
    successful_emulator = FakeEmulator()
    result = run_reconnect_preflight(
        successful_serial,  # type: ignore[arg-type]
        successful_emulator,  # type: ignore[arg-type]
        1,
        pre_stop_fence_timeout_s=0.5,
    )
    assert_true(result["serial_fence_observed"] is True, f"custom fence failed: {result}")
    assert_true(
        successful_serial.protocol_timeouts == [0.5, 5.0],
        "custom timeout escaped the successful pre-stop fence: "
        f"{successful_serial.protocol_timeouts}",
    )

    timed_out_serial = FakeSerial(timeout_first_fence=True)
    timed_out_emulator = FakeEmulator()
    try:
        run_reconnect_preflight(
            timed_out_serial,  # type: ignore[arg-type]
            timed_out_emulator,  # type: ignore[arg-type]
            1,
            pre_stop_fence_timeout_s=0.5,
        )
    except ReconnectPreflightFailure as exc:
        assert_true(
            exc.classification == "COLLECTION_FAILED",
            f"pre-stop timeout became product behavior: {exc.result}",
        )
        assert_true(
            exc.failure_kind == "evidence_or_transport",
            f"pre-stop timeout received the wrong taxonomy: {exc.failure_kind}",
        )
        assert_true(
            exc.result["serial_fence_observed"] is False,
            f"timed-out fence was recorded as observed: {exc.result}",
        )
    else:
        raise AssertionError("pre-stop fence timeout passed")
    assert_true(
        timed_out_serial.protocol_timeouts == [0.5],
        f"stress timeout was not forwarded: {timed_out_serial.protocol_timeouts}",
    )
    assert_true(
        timed_out_emulator.finish_calls == 1,
        "ordinary pre-stop failure changed emulator terminalization",
    )


def test_reconnect_preflight_pre_stop_fence_timeout_rejects_invalid_before_start() -> None:
    class NeverStartEmulator:
        def __init__(self) -> None:
            self.started = False

        def start(self) -> None:
            self.started = True

    for invalid in (0, -0.1, True, float("nan"), float("inf"), "0.5"):
        emulator = NeverStartEmulator()
        try:
            run_reconnect_preflight(
                SimpleNamespace(boot_marker_count=0, disconnect_cleanup_count=0),
                emulator,  # type: ignore[arg-type]
                1,
                pre_stop_fence_timeout_s=invalid,  # type: ignore[arg-type]
            )
        except ValueError as exc:
            assert_true("finite positive" in str(exc), f"wrong fence timeout error: {exc}")
        else:
            raise AssertionError(f"invalid pre-stop fence timeout passed: {invalid!r}")
        assert_true(not emulator.started, "invalid fence timeout started the emulator")


def test_reconnect_preflight_propagates_interruption_without_terminalizing_emulator() -> None:
    boundaries: list[str] = []

    class FakeSerial:
        boot_marker_count = 0
        disconnect_cleanup_count = 0

        def record_host_boundary(self, label: str) -> None:
            boundaries.append(label)

    class InterruptingEmulator:
        def __init__(self) -> None:
            self.started = False
            self.finished = False
            self.stopped = False

        def start(self) -> None:
            self.started = True

        def wait_for_handshake_ready(self, _timeout: float) -> None:
            raise InterruptedError("received signal 2")

        def finish_preflight(
            self,
            _handshake_ready_while_alive: bool,
        ) -> dict[str, object]:
            self.finished = True
            raise AssertionError("interrupted preflight was terminalized")

        def stop(self) -> None:
            self.stopped = True

    emulator = InterruptingEmulator()
    try:
        run_reconnect_preflight(
            FakeSerial(),  # type: ignore[arg-type]
            emulator,  # type: ignore[arg-type]
            1,
        )
    except InterruptedError as exc:
        assert_true(str(exc) == "received signal 2", f"interruption changed: {exc}")
    except ReconnectPreflightFailure as exc:
        raise AssertionError(f"interruption was classified as reconnect evidence: {exc}") from exc
    else:
        raise AssertionError("interrupted reconnect preflight returned")

    assert_true(emulator.started, "interruption fixture never entered the preflight")
    assert_true(not emulator.finished, "inner preflight finished an interrupted emulator")
    assert_true(not emulator.stopped, "inner preflight stopped an interrupted emulator")
    assert_true(
        boundaries == ["reconnect_preflight_start"],
        f"interruption invented terminal boundaries: {boundaries}",
    )

    # Model the stress runner's outer finally block, which remains the sole
    # cleanup owner after the interruption escapes.
    emulator.stop()
    assert_true(emulator.stopped, "outer cleanup could not stop the emulator")


def test_reconnect_preflight_orders_fence_stop_cleanup_and_second_fence() -> None:
    """Public behavior ID: V1-RECONNECT-SESSION-001."""
    events: list[str] = []

    class FakeSerial:
        boot_marker_count = 0
        disconnect_cleanup_count = 0

        def write_command(self, command: str) -> None:
            events.append(command)

        def read_protocol_line(self, _prefixes: tuple[str, ...], _timeout: float) -> str:
            events.append("QRESP")
            return 'QRESP {"ok":true,"state":"idle","suite":"core","mode":"current"}'

        def read_line(self, _timeout: float) -> str:
            events.append("cleanup")
            self.disconnect_cleanup_count += 1
            return "[BLE] V1 disconnected; cleared LCD BLE state at 123 ms"

        def record_host_boundary(self, label: str) -> None:
            events.append(label)

    class FakeEmulator:
        process = object()

        def start(self) -> None:
            events.append("start-a")

        def wait_for_handshake_ready(self, _timeout: float) -> None:
            events.append("ready-a")

        def health_problem(self) -> str:
            return ""

        def _bench_event(self, state: str) -> dict[str, object]:
            return {"state": state, "active": True}

        def finish_preflight(self, handshake_ready_while_alive: bool) -> dict[str, object]:
            events.append("stop-a")
            return {
                "handshake_ready_while_alive": handshake_ready_while_alive,
                "managed_stop": True,
                "confirmed_exit": True,
            }

    result = run_reconnect_preflight(FakeSerial(), FakeEmulator(), 1)
    assert_true(result["cleanup_marker_count"] == 1, f"wrong reconnect result: {result}")
    assert_true(
        events == [
            "reconnect_preflight_start", "start-a", "ready-a",
            "reconnect_preflight_fence_begin",
            "QSTATUS", "QRESP", "reconnect_preflight_fence_complete", "stop-a",
            "reconnect_preflight_process_exited", "cleanup",
            "reconnect_post_cleanup_fence_begin", "QSTATUS", "QRESP",
            "reconnect_post_cleanup_fence_complete",
        ],
        f"reconnect lifecycle reordered: {events}",
    )


def test_reconnect_serial_fence_requires_safe_status_shape() -> None:
    class FakeSerial:
        def __init__(self, response: str):
            self.response = response
            self.commands: list[str] = []

        def write_command(self, command: str) -> None:
            self.commands.append(command)

        def read_protocol_line(self, _prefixes: tuple[str, ...], _timeout: float) -> str:
            return self.response

    valid = FakeSerial(
        'QRESP {"ok":true,"state":"idle","suite":"core","mode":"current"}'
    )
    result = establish_serial_fence(valid)
    assert_true(result["state"] == "idle", f"safe status fence failed: {result}")
    assert_true(valid.commands == ["QSTATUS"], f"wrong fence command: {valid.commands}")

    for response in (
        'QRESP {"ok":true}',
        'QRESP {"ok":true,"state":"running","suite":"display","mode":"current"}',
    ):
        try:
            establish_serial_fence(FakeSerial(response))
        except RuntimeError as exc:
            assert_true("was not ready" in str(exc), f"wrong fence error: {exc}")
        else:
            raise AssertionError(f"unsafe serial fence passed: {response}")


def test_reconnect_readiness_uses_unique_fifo_barrier_before_status_fence() -> None:
    nonce = "0123456789abcdef0123456789abcdef"

    class FakeSerial:
        def __init__(self, lines: list[object], status: str = "idle") -> None:
            self.lines = list(lines)
            self.status = status
            self.commands: list[str] = []

        def write_command(self, command: str) -> None:
            self.commands.append(command)

        def read_line(self, _timeout: float) -> str:
            if not self.lines:
                raise TimeoutError("no barrier response")
            item = self.lines.pop(0)
            if isinstance(item, BaseException):
                raise item
            assert isinstance(item, str)
            return item

        def read_protocol_line(self, _prefixes: tuple[str, ...], _timeout: float) -> str:
            return (
                'QRESP {"ok":true,"state":"'
                + self.status
                + '","suite":"core","mode":"current"}'
            )

    for status in ("ready", "busy"):
        delayed = [
            'QRESP {"ok":true,"state":"idle","suite":"core","mode":"current"}',
            'QRESP {"ok":true,"state":"idle","suite":"core","mode":"current"}',
            f'QBSC08 {{"schema":1,"nonce":"{nonce}","status":"{status}"}}',
        ]
        serial = FakeSerial(delayed)
        result = establish_reconnect_readiness(serial, 1, nonce=nonce)
        assert_true(result["state"] == "idle", f"{status} barrier failed: {result}")
        assert_true(
            serial.commands == [f"QBSC08 {nonce}", "QSTATUS"],
            f"{status} barrier did not order the final fence: {serial.commands}",
        )

    invalid_lines = (
        (f'QBSC08 {{"schema":1,"nonce":"{"f" * 32}","status":"ready"}}', "wrong nonce"),
        ('QBSC08 {"schema":1,"status":"ready"}', "wrong nonce"),
        ("QBSC08 {bad", "malformed"),
        ('QRESP {bad', "malformed delayed QRESP"),
        ('QERR {"ok":false,"error":"bsc08_provider_missing"}', "received QERR"),
    )
    for line, expected in invalid_lines:
        try:
            establish_reconnect_readiness(FakeSerial([line]), 0.05, nonce=nonce)
        except RuntimeError as exc:
            assert_true(expected in str(exc), f"wrong barrier error for {line!r}: {exc}")
        else:
            raise AssertionError(f"invalid readiness barrier passed: {line}")

    try:
        establish_reconnect_readiness(FakeSerial([]), 0.01, nonce=nonce)
    except RuntimeError as exc:
        assert_true("timed out" in str(exc), f"wrong missing-barrier error: {exc}")
    else:
        raise AssertionError("missing readiness barrier passed")


def test_reconnect_preflight_failure_retains_terminal_result() -> None:
    class FakeSerial:
        boot_marker_count = 0
        disconnect_cleanup_count = 0

        def write_command(self, _command: str) -> None:
            pass

        def read_protocol_line(self, _prefixes: tuple[str, ...], _timeout: float) -> str:
            return 'QRESP {"ok":true,"state":"idle","suite":"core","mode":"current"}'

        def read_line(self, _timeout: float) -> str:
            raise TimeoutError("no cleanup")

        def record_host_boundary(self, _label: str) -> None:
            pass

    class FakeEmulator:
        process = object()

        def start(self) -> None:
            pass

        def wait_for_handshake_ready(self, _timeout: float) -> None:
            pass

        def health_problem(self) -> str:
            return ""

        def _bench_event(self, _state: str) -> dict[str, object]:
            return {"active": True}

        def finish_preflight(self, handshake_ready_while_alive: bool) -> dict[str, object]:
            return {
                "handshake_ready_while_alive": handshake_ready_while_alive,
                "managed_stop": True,
                "confirmed_exit": True,
            }

    try:
        run_reconnect_preflight(FakeSerial(), FakeEmulator(), 0.01)
    except ReconnectPreflightFailure as exc:
        assert_true(exc.classification == "FAIL", f"wrong no-cleanup taxonomy: {exc.classification}")
        assert_true(exc.failure_kind == "cleanup_missing", f"wrong failure kind: {exc.failure_kind}")
        assert_true(exc.result["handshake_ready_while_alive"] is True, f"lost readiness: {exc.result}")
        assert_true(exc.result["managed_stop"] is True, f"lost managed stop: {exc.result}")
        assert_true(exc.result["confirmed_exit"] is True, f"lost exit result: {exc.result}")
        assert_true(exc.result["cleanup_marker_count"] == 0, f"invented cleanup: {exc.result}")
    else:
        raise AssertionError("missing cleanup marker passed reconnect preflight")


def test_reconnect_preflight_distinguishes_behavior_from_broken_evidence() -> None:
    class HealthySerial:
        boot_marker_count = 0
        disconnect_cleanup_count = 0

        def write_command(self, _command: str) -> None:
            pass

        def read_protocol_line(self, _prefixes: tuple[str, ...], _timeout: float) -> str:
            return 'QRESP {"ok":true,"state":"idle","suite":"core","mode":"current"}'

        def read_line(self, _timeout: float) -> str:
            raise TimeoutError("no line")

        def record_host_boundary(self, _label: str) -> None:
            pass

    class BaseEmulator:
        process = object()

        def start(self) -> None:
            pass

        def health_problem(self) -> str:
            return ""

        def finish_preflight(self, handshake_ready_while_alive: bool) -> dict[str, object]:
            return {
                "handshake_ready_while_alive": handshake_ready_while_alive,
                "managed_stop": True,
                "confirmed_exit": True,
            }

    class TimeoutEmulator(BaseEmulator):
        def wait_for_handshake_ready(self, _timeout: float) -> None:
            raise ReconnectBehaviorError("handshake_timeout", "incomplete handshake")

    class InvalidHandshakeEmulator(BaseEmulator):
        def wait_for_handshake_ready(self, _timeout: float) -> None:
            raise ReconnectBehaviorError("handshake_invalid", "wrong literal or route")

    try:
        run_reconnect_preflight(HealthySerial(), TimeoutEmulator(), 0.01)
    except ReconnectPreflightFailure as exc:
        assert_true(exc.classification == "FAIL", f"healthy timeout was inconclusive: {exc.result}")
        assert_true(exc.failure_kind == "handshake_timeout", f"wrong timeout kind: {exc.failure_kind}")
    else:
        raise AssertionError("incomplete handshake passed")

    try:
        run_reconnect_preflight(HealthySerial(), InvalidHandshakeEmulator(), 0.01)
    except ReconnectPreflightFailure as exc:
        assert_true(
            exc.classification == "FAIL" and exc.failure_kind == "handshake_invalid",
            f"valid-but-wrong handshake was not actionable: {exc.result}",
        )
    else:
        raise AssertionError("valid-but-wrong handshake passed")

    for label, message in (
        ("malformed ledger", "invalid preflight ledger"),
        ("early process death", "V1 emulator exited early"),
    ):
        class BrokenEmulator(BaseEmulator):
            def wait_for_handshake_ready(self, _timeout: float) -> None:
                raise RuntimeError(message)

        try:
            run_reconnect_preflight(HealthySerial(), BrokenEmulator(), 0.01)
        except ReconnectPreflightFailure as exc:
            assert_true(
                exc.classification == "COLLECTION_FAILED",
                f"{label} was treated as a product failure: {exc.result}",
            )
        else:
            raise AssertionError(f"{label} passed")

    class SerialFailure(HealthySerial):
        def read_protocol_line(self, _prefixes: tuple[str, ...], _timeout: float) -> str:
            raise OSError("serial disconnected")

    class ReadyEmulator(BaseEmulator):
        def wait_for_handshake_ready(self, _timeout: float) -> None:
            pass

        def _bench_event(self, _state: str) -> dict[str, object]:
            return {"active": True}

    try:
        run_reconnect_preflight(SerialFailure(), ReadyEmulator(), 0.01)
    except ReconnectPreflightFailure as exc:
        assert_true(
            exc.classification == "COLLECTION_FAILED",
            f"serial failure was treated as product behavior: {exc.result}",
        )
    else:
        raise AssertionError("serial failure passed")


def test_reconnect_preflight_process_uses_separate_quiet_artifacts() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        executable = root / "v1replay"
        write_dummy_emulator(
            executable,
            emit_complete=False,
            blink_profile="scenario",
            blink_samples=57,
        )
        emulator = V1Emulator(
            executable,
            root / "replay",
            "replay",
            "scenario",
            handshake_only=True,
        )
        try:
            emulator.start()
            expected = (
                f"argv=bench --machine-events --owner-pid {os.getpid()} "
                "--blink-profile scenario --handshake-only "
                "--log-packets "
                f"--handshake-ledger {root / 'replay' / RECONNECT_LEDGER_NAME}"
            )
            deadline = time.monotonic() + 1
            log = ""
            while time.monotonic() < deadline:
                log = (root / "replay" / RECONNECT_LOG_NAME).read_text(encoding="utf-8")
                if expected in log:
                    break
                time.sleep(0.02)
            assert_true(expected in log, f"preflight argv was not isolated: {log!r}")
        finally:
            emulator.stop()


def test_handshake_ledger_runner_and_delivery_wiring_are_pinned() -> None:
    runner = (ROOT / "scripts" / "bench" / "run_window.py").read_text(encoding="utf-8")
    peripheral = (
        ROOT / "tools" / "v1replay" / "Sources" / "v1replay" / "Peripheral.swift"
    ).read_text(encoding="utf-8")
    ledger = (
        ROOT / "tools" / "v1replay" / "Sources" / "v1replay" / "HandshakeLedger.swift"
    ).read_text(encoding="utf-8")

    assert_true(
        'HANDSHAKE_LEDGER_NAME = "handshake_ledger.jsonl"' in runner,
        "runner does not own the bounded handshake-ledger artifact name",
    )
    assert_true(
        'command.extend(["--handshake-ledger", str(self.handshake_ledger_path)])' in runner,
        "managed replay does not pass its same-window handshake-ledger path",
    )
    assert_true(
        '"handshake_ledger_path": (' in runner
        and 'str(out_dir / HANDSHAKE_LEDGER_NAME) if args.suite == "replay" else ""' in runner,
        "replay window_result does not retain the same-window handshake ledger",
    )
    initial_ready = runner.index("ready = wait_ready(q, args.ready_timeout_seconds)")
    barrier = runner.index(
        "ready = establish_reconnect_readiness(q, args.ready_timeout_seconds)",
        initial_ready,
    )
    preflight_start = runner.index(
        "reconnect_preflight_result = run_reconnect_preflight(",
        barrier,
    )
    assert_true(
        initial_ready < barrier < preflight_start,
        "reconnect evidence starts before delayed readiness replies cross the nonce barrier",
    )

    update = peripheral.index("guard manager.updateValue(")
    dequeue = peripheral.index("pending.removeFirst()", update)
    delivery = peripheral.index("handshakeLedger?.recordDelivered(", update)
    assert_true(
        update < dequeue < delivery,
        "peripheral credits notification evidence before CoreBluetooth accepts delivery",
    )

    assert_true(
        "static let maximumEpochs = 4" in ledger
        and "static let maximumEventsPerEpoch = 12" in ledger,
        "handshake evidence is not bounded to four anonymous twelve-event epochs",
    )
    assert_true(
        "guard !streamRecordedEpochs.contains(queuedEpoch) else { return }" in ledger
        and "streamRecordedEpochs.insert(queuedEpoch)" in ledger,
        "ledger does not retain only the first delivered alert row per epoch",
    )
    assert_true(
        "addedSecondShortSubscriber = inserted && shortSubscriberIDs.count > 1" in peripheral
        and "else if addedSecondShortSubscriber" in peripheral,
        "a second short-channel central can be spliced into the active anonymous epoch",
    )
    assert_true(
        "endedShortSession = removed && shortSubscriberIDs.isEmpty" in peripheral
        and "if endedShortSession || remaining == 0" in peripheral,
        "loss of the required B2CE subscription does not end its anonymous epoch",
    )
    assert_true("UUID" not in ledger and "timestamp" not in ledger, "ledger stores private identity or time")


def test_global_shutter_default_uses_qualified_720p200_profile() -> None:
    previous = os.environ.pop("BENCH_CAMERA_FRAMERATE", None)
    try:
        with tempfile.TemporaryDirectory() as tmp:
            camera = CameraCapture(Path(tmp), 300)
            assert_true(camera.camera_name == "Global Shutter Camera", f"wrong camera: {camera.camera_name}")
            assert_true(camera.framerate == 200, f"unsupported default camera rate: {camera.framerate}")
            assert_true(camera.video_size == "1280x720", f"wrong camera size: {camera.video_size}")
            assert_true(
                camera.input_pixel_format == "nv12",
                f"wrong camera input pixel format: {camera.input_pixel_format}",
            )
            assert_true(camera.focus == 306, f"wrong fixed focus: {camera.focus}")
            assert_true(
                camera.capture_backend == "avfoundation_native",
                f"wrong capture backend: {camera.capture_backend}",
            )
    finally:
        if previous is not None:
            os.environ["BENCH_CAMERA_FRAMERATE"] = previous


def test_native_camera_recorder_uses_host_clock_timeline() -> None:
    source = (ROOT / "scripts" / "bench" / "camera_recorder.swift").read_text(encoding="utf-8")
    runner = (ROOT / "scripts" / "bench" / "run_window.py").read_text(encoding="utf-8")
    assert_true(
        "videoOutput.alwaysDiscardsLateVideoFrames = true" in source,
        "native recorder can accumulate stale camera frames",
    )
    assert_true(
        "CMClockGetTime(CMClockGetHostTimeClock())" in source
        and "pixelBufferAdaptor.append(pixelBuffer, withPresentationTime: presentationTime)" in source
        and "writer.startSession(atSourceTime: .zero)" in source,
        "native recorder does not replace camera timestamps with a monotonic host timeline",
    )
    assert_true(
        "writerInput.append(sampleBuffer)" not in source,
        "native recorder still forwards camera-owned sample timing to AVAssetWriter",
    )
    assert_true(
        "switch writer.status" in source
        and 'code: "writer_failed"' in source
        and "--self-test-writer" in source,
        "native writer failure can hide behind backpressure or lacks a real writer-path test",
    )
    assert_true(
        'if camera is None or args.suite != "replay"' in runner
        and runner.count("require_healthy_replay_camera()") == 3,
        "diagnostic core/display camera health can alter product collection",
    )


def test_camera_failure_marker_aborts_active_window() -> None:
    class FakeSerial:
        def __init__(self) -> None:
            self.commands: list[str] = []

        def write_command(self, command: str) -> None:
            self.commands.append(command)

        def read_protocol_line(self, _prefixes: tuple[str, ...], _timeout: float) -> str:
            if self.commands[-1] == "QABORT":
                return (
                    'QRESP {"ok":false,"state":"error","suite":"core",'
                    '"message":"aborted","error":"aborted"}'
                )
            return (
                'QRESP {"ok":true,"state":"running","suite":"core",'
                '"csvPath":"/perf/test.csv"}'
            )

    class ExitedRecorder:
        returncode = 7

        def poll(self) -> int:
            return self.returncode

    with tempfile.TemporaryDirectory() as tmp:
        camera = CameraCapture(Path(tmp), 300)
        camera.out_dir.mkdir(parents=True, exist_ok=True)
        camera.failure_marker_path.write_text(
            json.dumps(
                {
                    "schema_version": 1,
                    "result": "CAPTURE_FAILED",
                    "code": "frame_append_failed",
                    "message": "movie frame append failed",
                    "error": {
                        "domain": "AVFoundationErrorDomain",
                        "code": -11800,
                        "underlying": {"domain": "NSOSStatusErrorDomain", "code": -16364},
                    },
                }
            )
            + "\n",
            encoding="utf-8",
        )
        camera.process = ExitedRecorder()  # type: ignore[assignment]
        serial = FakeSerial()

        def require_healthy_camera() -> str:
            problem = camera.health_problem()
            if problem:
                raise run_window_module.CameraEvidenceFailure(problem, camera)
            return ""

        try:
            start_and_wait(
                serial,  # type: ignore[arg-type]
                "replay",
                1,
                1,
                health_check=require_healthy_camera,
            )
        except run_window_module.CameraEvidenceFailure as exc:
            message = str(exc)
            assert_true("frame_append_failed" in message, f"failure code was lost: {message}")
            assert_true(
                "AVFoundationErrorDomain -11800" in message,
                f"recorder error identity was lost: {message}",
            )
            assert_true(
                "NSOSStatusErrorDomain -16364" in message,
                f"underlying recorder error identity was lost: {message}",
            )
            assert_true("exited during capture" not in message, f"generic exit hid marker: {message}")
        else:
            raise AssertionError("camera writer failure did not abort the active window")
        assert_true(
            serial.commands == ["QSTART core 1", "QABORT"],
            f"camera failure left the DUT window active: {serial.commands}",
        )


def test_live_camera_failure_is_serialized_as_evidence_failure() -> None:
    original_parse_args = run_window_module.parse_args
    original_collect_live = run_window_module.collect_live
    original_build_identity = run_window_module.build_identity_manifest
    original_install_signals = run_window_module.install_signal_handlers

    with tempfile.TemporaryDirectory() as tmp:
        out_dir = Path(tmp) / "replay"
        args = SimpleNamespace(
            suite="replay",
            duration_seconds=300,
            profile="drive_wifi_off",
            segment="last",
            blink_profile=None,
            blink_arrow=False,
            out_dir=str(out_dir),
            identity_manifest=str(out_dir / "identity.json"),
            board_id="release",
            git_sha="1" * 40,
            git_ref="main",
            git_worktree_clean="1",
            post_upload_settle_seconds=0,
            from_csv="",
            replay_executable="fixture-v1replay",
            camera=True,
            upload=False,
        )
        identity = {
            "product_fingerprint": "a" * 64,
            "grader_fingerprint": "b" * 64,
            "hardware_scoring_fingerprint": "c" * 64,
            "scenario_fingerprint": "d" * 64,
        }

        def fail_with_camera_evidence(
            _args: object,
            target: Path,
            **_kwargs: object,
        ) -> object:
            camera = CameraCapture(target / "camera", 300)
            camera.recorder_failure = {
                "schema_version": 1,
                "result": "CAPTURE_FAILED",
                "code": "frame_append_failed",
                "message": "movie frame append failed",
                "error": {
                    "domain": "AVFoundationErrorDomain",
                    "code": -11800,
                    "underlying": {"domain": "NSOSStatusErrorDomain", "code": -16364},
                },
            }
            message = camera._recorder_failure_message(camera.recorder_failure)
            camera.errors.append(message)
            camera._write_result("CAPTURE_FAILED")
            failure = run_window_module.CameraEvidenceFailure(message, camera)
            failure.reconnect_preflight = {"result": "PASS"}
            raise failure

        try:
            run_window_module.parse_args = lambda: args  # type: ignore[assignment]
            run_window_module.collect_live = fail_with_camera_evidence  # type: ignore[assignment]
            run_window_module.build_identity_manifest = (  # type: ignore[assignment]
                lambda *_args, **_kwargs: dict(identity)
            )
            run_window_module.install_signal_handlers = lambda: None  # type: ignore[assignment]
            returncode = run_window_module.main()
        finally:
            run_window_module.parse_args = original_parse_args
            run_window_module.collect_live = original_collect_live
            run_window_module.build_identity_manifest = original_build_identity
            run_window_module.install_signal_handlers = original_install_signals

        window = json.loads((out_dir / "window_result.json").read_text(encoding="utf-8"))
        assert_true(returncode == 3, f"camera evidence failure exit={returncode}")
        assert_true(window["result"] == "EVIDENCE_FAILED", f"wrong taxonomy: {window}")
        assert_true(window["camera_failure_stage"] == "recording", f"stage lost: {window}")
        assert_true(window["camera_failure_kind"] == "frame_append_failed", f"code lost: {window}")
        assert_true(
            window["camera"]["recorder_failure"]["error"]["underlying"]["code"] == -16364,
            f"camera error identity was lost: {window}",
        )


def test_camera_stop_timeout_exceeds_native_finalize_timeout() -> None:
    waits: list[float] = []
    signals: list[int] = []

    class FakeProcess:
        pid = 1234

        def __init__(self) -> None:
            self.returncode: int | None = None

        def poll(self) -> int | None:
            return self.returncode

        def wait(self, timeout: float) -> int:
            waits.append(timeout)
            self.returncode = 0
            return 0

    original_killpg = camera_capture_module.os.killpg
    try:
        camera_capture_module.os.killpg = lambda _pid, sig: signals.append(sig)  # type: ignore[assignment]
        with tempfile.TemporaryDirectory() as tmp:
            camera = CameraCapture(Path(tmp), 300)
            camera.process = FakeProcess()  # type: ignore[assignment]
            camera._stop_process()
    finally:
        camera_capture_module.os.killpg = original_killpg

    assert_true(
        camera_capture_module.CAMERA_PROCESS_STOP_TIMEOUT_S
        > camera_capture_module.CAMERA_RECORDER_FINALIZE_TIMEOUT_S,
        "host can kill the recorder before its native finalization deadline",
    )
    assert_true(
        camera_capture_module.CAMERA_PREFLIGHT_FINISHED_TIMEOUT_S
        > camera_capture_module.CAMERA_NATIVE_PREFLIGHT_FINALIZE_TIMEOUT_S,
        "host can time out before native preflight finalization publishes its marker",
    )
    assert_true(
        camera_capture_module.CAMERA_SESSION_READY_TIMEOUT_S >= 45,
        "cold Swift module compilation can outlive camera session admission",
    )
    assert_true(
        run_logged_module.MANAGED_SHUTDOWN_GRACE_SECONDS
        > camera_capture_module.CAMERA_PROCESS_STOP_TIMEOUT_S + 20,
        "the managed wrapper can kill run_window before camera cleanup completes",
    )
    bench_source = (ROOT / "bench.sh").read_text(encoding="utf-8")
    assert_true(
        "WRAPPER_SHUTDOWN_GRACE_SECONDS=75" in bench_source
        and "shutdown_deadline=$((SECONDS + WRAPPER_SHUTDOWN_GRACE_SECONDS))" in bench_source,
        "the outer bench wrapper can preempt managed run cleanup",
    )
    run_logged_source = (ROOT / "scripts" / "bench" / "run_logged.py").read_text(
        encoding="utf-8"
    )
    assert_true(
        "if interrupted_status:\n            return" in run_logged_source,
        "repeated signals can extend the managed cleanup deadline",
    )
    assert_true(
        "if interrupted_status and not termination_forwarded and process.poll() is None"
        in run_logged_source,
        "a signal received during managed-process startup can be lost",
    )
    assert_true(
        waits == [camera_capture_module.CAMERA_PROCESS_STOP_TIMEOUT_S],
        f"camera stop used the wrong deadline: {waits}",
    )
    assert_true(signals == [signal.SIGINT], f"graceful camera stop sent destructive signals: {signals}")


def test_run_logged_forwards_first_signal_received_during_startup() -> None:
    handlers: dict[int, object] = {}
    forwarded: list[int] = []
    fake_process: object | None = None

    class FakeProcess:
        pid = 2468

        def __init__(self) -> None:
            self.returncode: int | None = None
            self.stdout = io.BytesIO()
            self.stderr = io.BytesIO()

        def poll(self) -> int | None:
            return self.returncode

        def wait(self, timeout: float | None = None) -> int:
            del timeout
            if self.returncode is None:
                raise subprocess.TimeoutExpired("fixture", 0)
            return self.returncode

    original_parse_args = run_logged_module.parse_args
    original_popen = run_logged_module.subprocess.Popen
    original_signal = run_logged_module.signal.signal
    original_killpg = run_logged_module.os.killpg
    try:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            run_logged_module.parse_args = lambda: SimpleNamespace(  # type: ignore[assignment]
                stdout=str(root / "stdout.log"),
                stderr=str(root / "stderr.log"),
                combined=str(root / "combined.log"),
                command=["fixture"],
            )
            run_logged_module.signal.signal = (  # type: ignore[assignment]
                lambda signum, handler: handlers.__setitem__(signum, handler)
            )

            def fake_popen(*_args: object, **_kwargs: object) -> FakeProcess:
                nonlocal fake_process
                process = FakeProcess()
                fake_process = process
                handler = handlers[signal.SIGTERM]
                assert callable(handler)
                handler(signal.SIGTERM, None)
                handler(signal.SIGINT, None)
                return process

            def fake_killpg(_pid: int, sent_signal: int) -> None:
                forwarded.append(sent_signal)
                assert isinstance(fake_process, FakeProcess)
                fake_process.returncode = 0

            run_logged_module.subprocess.Popen = fake_popen  # type: ignore[assignment]
            run_logged_module.os.killpg = fake_killpg  # type: ignore[assignment]
            returncode = run_logged_module.main()
    finally:
        run_logged_module.parse_args = original_parse_args
        run_logged_module.subprocess.Popen = original_popen
        run_logged_module.signal.signal = original_signal
        run_logged_module.os.killpg = original_killpg

    assert_true(returncode == 143, f"first signal status was not retained: {returncode}")
    assert_true(
        forwarded == [signal.SIGTERM],
        f"startup/repeated signals were lost or multiply forwarded: {forwarded}",
    )


def test_camera_probe_failure_retains_sanitized_diagnostics() -> None:
    class FailedProbe:
        returncode = 2
        stdout = ""
        stderr = "moov atom not found in private artifact path"

    original_run = camera_capture_module.subprocess.run
    try:
        camera_capture_module.subprocess.run = lambda *_args, **_kwargs: FailedProbe()  # type: ignore[assignment]
        with tempfile.TemporaryDirectory() as tmp:
            camera = CameraCapture(Path(tmp), 300)
            camera.ffprobe = "ffprobe"
            try:
                camera._probe_video()
            except RuntimeError as exc:
                message = str(exc)
                assert_true("ffprobe exit 2" in message, f"probe exit status was lost: {message}")
                assert_true("moov atom not found" in message, f"probe diagnostic was lost: {message}")
            else:
                raise AssertionError("failed video probe passed")
    finally:
        camera_capture_module.subprocess.run = original_run


def test_early_recorder_exit_is_latched_as_capture_failure() -> None:
    class ExitedRecorder:
        returncode = 0

        def poll(self) -> int:
            return self.returncode

    with tempfile.TemporaryDirectory() as tmp:
        camera = CameraCapture(Path(tmp), 300)
        camera.process = ExitedRecorder()  # type: ignore[assignment]
        problem = camera.health_problem()
        assert_true("exited during capture (code 0)" in problem, f"exit was hidden: {problem}")
        assert_true(camera.recorder_failure.get("code") == "recorder_exited_early", str(camera.recorder_failure))
        assert_true(problem in camera.errors, f"early exit was not latched: {camera.errors}")
        camera._write_result("CAPTURE_FAILED")
        raw = json.loads(camera.result_path.read_text(encoding="utf-8"))
        assert_true(raw["result"] == "CAPTURE_FAILED", f"raw artifact contradicted failure: {raw}")
        assert_true(raw["recorder_failure"]["returncode"] == 0, f"return code was lost: {raw}")


def test_unadmitted_camera_is_not_reported_as_capture_failure() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        camera = CameraCapture(Path(tmp), 300)
        result = camera.stop(collection_completed=False)
        assert_true(result["result"] == "NOT_RUN", f"unadmitted camera result: {result}")
        assert_true(
            result.get("reason") == "camera admission was not reached",
            f"unadmitted camera reason is missing: {result}",
        )
        assert_true(
            not camera.result_path.exists(),
            "unadmitted camera synthesized a physical capture artifact",
        )

        failed_start = {"result": "CAPTURE_FAILED", "errors": ["camera unavailable"]}
        camera.result_path.write_text(json.dumps(failed_start) + "\n", encoding="utf-8")
        assert_true(
            camera.stop(collection_completed=False) == failed_start,
            "an admitted camera failure was replaced with NOT_RUN",
        )


def test_reconnect_failure_before_camera_admission_leaves_no_camera_artifact() -> None:
    observed_stop_results: list[dict[str, object]] = []

    class FakeSerial:
        boot_marker_count = 0
        disconnect_cleanup_count = 0

        def __init__(self, *_args: object, **_kwargs: object) -> None:
            pass

        def close(self) -> None:
            pass

    class FakeEmulator:
        process = None

        def __init__(self, *_args: object, **_kwargs: object) -> None:
            pass

        def finish(self, _completed: bool) -> dict[str, object]:
            return {"completed": False, "mode": "bench"}

    class RecordingCamera(CameraCapture):
        def stop(self, collection_completed: bool) -> dict[str, object]:
            result = super().stop(collection_completed)
            observed_stop_results.append(result)
            return result

    class FakeLease:
        fd = 42

        def __enter__(self) -> FakeLease:
            return self

        def __exit__(self, *_args: object) -> None:
            pass

    def fail_reconnect(*_args: object, **_kwargs: object) -> dict[str, object]:
        raise ReconnectPreflightFailure(
            "duplicate start request",
            {
                "handshake_ready_while_alive": False,
                "serial_fence_observed": True,
                "managed_stop": True,
                "confirmed_exit": True,
                "cleanup_marker_count": 0,
                "serial_session_continuous": True,
                "boot_observed_before_second_complete": False,
            },
            classification="FAIL",
            failure_kind="handshake_invalid",
        )

    originals = (
        run_window_module.wait_for_port,
        run_window_module.BenchSerial,
        run_window_module.wait_ready,
        run_window_module.establish_reconnect_readiness,
        run_window_module.V1Emulator,
        run_window_module.CameraCapture,
        run_window_module.V1RadioLease,
        run_window_module.run_reconnect_preflight,
        run_window_module.run_camera_preflight,
    )
    try:
        run_window_module.wait_for_port = lambda *_args, **_kwargs: "fake-port"
        run_window_module.BenchSerial = FakeSerial
        run_window_module.wait_ready = lambda *_args, **_kwargs: {"ok": True}
        run_window_module.establish_reconnect_readiness = lambda *_args, **_kwargs: {"ok": True}
        run_window_module.V1Emulator = FakeEmulator
        run_window_module.CameraCapture = RecordingCamera
        run_window_module.V1RadioLease = FakeLease
        run_window_module.run_reconnect_preflight = fail_reconnect
        run_window_module.run_camera_preflight = lambda *_args, **_kwargs: (_ for _ in ()).throw(
            AssertionError("camera preflight ran before reconnect admission")
        )
        with tempfile.TemporaryDirectory() as tmp:
            out_dir = Path(tmp) / "replay"
            args = SimpleNamespace(
                suite="replay",
                camera=True,
                upload=False,
                port="fake-port",
                baud=115200,
                replay_executable=str(Path(tmp) / "v1replay"),
                blink_profile="scenario",
                ready_timeout_seconds=1,
                duration_seconds=300,
            )
            try:
                run_window_module.collect_live(args, out_dir)
            except ReconnectPreflightFailure as exc:
                assert_true(exc.failure_kind == "handshake_invalid", f"wrong failure: {exc}")
            else:
                raise AssertionError("invalid reconnect unexpectedly reached camera admission")
            assert_true(
                observed_stop_results
                == [{
                    "result": "NOT_RUN",
                    "reason": "camera admission was not reached",
                    "errors": [],
                }],
                f"unadmitted camera finalization changed: {observed_stop_results}",
            )
            assert_true(not (out_dir / "camera").exists(), "unadmitted camera directory was created")
    finally:
        (
            run_window_module.wait_for_port,
            run_window_module.BenchSerial,
            run_window_module.wait_ready,
            run_window_module.establish_reconnect_readiness,
            run_window_module.V1Emulator,
            run_window_module.CameraCapture,
            run_window_module.V1RadioLease,
            run_window_module.run_reconnect_preflight,
            run_window_module.run_camera_preflight,
        ) = originals


def test_camera_grader_integrates_high_speed_frames_before_sampling() -> None:
    commands: list[list[str]] = []

    class FakeStream:
        def __init__(self, chunks: list[bytes]) -> None:
            self.chunks = chunks

        def read(self, _size: int = -1) -> bytes:
            return self.chunks.pop(0) if self.chunks else b""

    class FakeProcess:
        def __init__(self, command: list[str], **_kwargs: object) -> None:
            commands.append(command)
            self.stdout = FakeStream([bytes(camera_grade_module.FRAME_BYTES), b""])
            self.stderr = FakeStream([b""])

        def wait(self) -> int:
            return 0

    original_popen = camera_grade_module.subprocess.Popen
    try:
        camera_grade_module.subprocess.Popen = FakeProcess  # type: ignore[assignment]
        observations = camera_grade_module.extract_observations(Path("evidence.mov"), ffmpeg="ffmpeg")
    finally:
        camera_grade_module.subprocess.Popen = original_popen
    assert_true(len(observations) == 1, f"fake camera frame was not graded: {observations}")
    video_filter = commands[0][commands[0].index("-vf") + 1]
    assert_true(
        f"tmix=frames={camera_grade_module.TEMPORAL_INTEGRATION_FRAMES}" in video_filter,
        f"high-speed frames are not integrated before grading: {video_filter}",
    )


def test_camera_profile_is_reapplied_after_recorder_open() -> None:
    events: list[str] = []

    class FakeProcess:
        returncode = None

        def poll(self) -> None:
            events.append("poll")
            return None

    class OrderingCameraCapture(CameraCapture):
        def _require_tools(self) -> None:
            pass

        def _configure(self, exposure: int) -> None:
            events.append(f"configure:{exposure}")

        def _extract_video_still(self, _video: Path, path: Path, _time_s: float) -> None:
            events.append("extract_preflight")
            path.write_bytes(b"snapshot")

        def _wait_for_marker(self, _path: Path, _timeout_s: float, label: str) -> dict[str, object]:
            events.append(f"ready:{label}")
            return {"result": "READY"}

        def _validate_live_profile(self) -> dict[str, int | bool]:
            events.append("validate_profile")
            return {}

    original_popen = camera_capture_module.subprocess.Popen
    original_sleep = camera_capture_module.time.sleep
    try:
        camera_capture_module.subprocess.Popen = lambda *args, **kwargs: (  # type: ignore[assignment]
            events.append("recorder_open") or FakeProcess()
        )
        camera_capture_module.time.sleep = lambda seconds: events.append(  # type: ignore[assignment]
            f"sleep:{seconds}"
        )
        with tempfile.TemporaryDirectory() as tmp:
            camera = OrderingCameraCapture(Path(tmp), 300)
            camera.swift = "swift"
            assert_true(camera.start(), f"camera start failed: {camera.errors}")
            recorder_index = events.index("recorder_open")
            configure_indices = [index for index, event in enumerate(events) if event.startswith("configure:")]
            assert_true(len(configure_indices) == 1, f"camera profile was not applied once: {events}")
            assert_true(
                recorder_index < configure_indices[0] < events.index("validate_profile"),
                f"camera profile was not reapplied after recorder ownership: {events}",
            )
            camera.process = None
            if camera.log_handle is not None:
                camera.log_handle.close()
                camera.log_handle = None
    finally:
        camera_capture_module.subprocess.Popen = original_popen
        camera_capture_module.time.sleep = original_sleep


def test_camera_video_profile_seeds_exposure_before_aperture_priority() -> None:
    class ControlCapture(CameraCapture):
        def __init__(self, out_dir: Path) -> None:
            super().__init__(out_dir, 300)
            self.controls: list[tuple[str, int]] = []

        def _set_control(self, name: str, value: int) -> None:
            self.controls.append((name, value))

    with tempfile.TemporaryDirectory() as tmp:
        camera = ControlCapture(Path(tmp))
        camera._configure(VIDEO_EXPOSURE)
        video = dict(camera.controls)
        assert_true(video["auto-exposure-mode"] == 8, f"video profile is not aperture priority: {video}")
        assert_true(video["gain"] == 0, f"video profile changed qualified gain: {video}")
        manual_index = camera.controls.index(("auto-exposure-mode", 1))
        exposure_index = camera.controls.index(("exposure-time-abs", VIDEO_EXPOSURE))
        aperture_index = camera.controls.index(("auto-exposure-mode", 8))
        assert_true(
            manual_index < exposure_index < aperture_index,
            f"video profile did not seed exposure before restoring aperture priority: {camera.controls}",
        )

def test_failed_frame_rate_probe_retains_measurements_and_diagnostics() -> None:
    class FinishedProcess:
        returncode = 0

        def poll(self) -> int:
            return 0

    class LowRateCameraCapture(CameraCapture):
        def _probe_video(self) -> dict[str, float | int]:
            return {
                "duration_seconds": 300.0,
                "width": 1280,
                "height": 720,
                "average_frame_rate": 14.917,
            }

        def _validate_recording_profile(self) -> dict[str, object]:
            return {"result": "PASS"}

        def _extract_video_still(self, _video: Path, path: Path, _time_s: float) -> None:
            path.write_bytes(b"snapshot")

        def _set_control(self, _name: str, _value: int) -> None:
            pass

        def _validate_live_profile(self) -> dict[str, int | bool]:
            return {}

    with tempfile.TemporaryDirectory() as tmp:
        camera = LowRateCameraCapture(Path(tmp), 300)
        camera.process = FinishedProcess()  # type: ignore[assignment]
        camera.video_path.write_bytes(b"video")
        camera.preflight_path.write_bytes(b"snapshot")

        result = camera.stop(collection_completed=True)

        assert_true(result["result"] == "CAPTURE_FAILED", f"low-rate capture passed: {result}")
        assert_true(
            result["video_probe"]["average_frame_rate"] == 14.917,
            f"measured frame rate was discarded: {result}",
        )
        assert_true(result["video_duration_seconds"] == 300.0, f"duration was discarded: {result}")
        assert_true(camera.bright_path.is_file(), f"bright diagnostic still is missing: {result}")
        assert_true(camera.dim_path.is_file(), f"dim diagnostic still is missing: {result}")
        assert_true(
            any("frame rate is below" in error for error in result["errors"]),
            f"frame-rate failure was not retained: {result}",
        )


def test_bench_entrypoint_forwards_explicit_baseline_window() -> None:
    entrypoint = (ROOT / "bench.sh").read_text(encoding="utf-8")
    assert_true('COMPARE_TO=()' in entrypoint, "bench entrypoint does not own an explicit baseline window")
    assert_true(
        'COMPARE_TO+=("$2")' in entrypoint,
        "bench entrypoint does not collect repeated --compare-to arguments",
    )
    assert_true(
        'args+=(--compare-to "$compare_to")' in entrypoint,
        "bench entrypoint does not forward explicit baselines to every selected window",
    )
    assert_true(
        '"${#COMPARE_TO[@]}" -eq 0' in entrypoint,
        "explicit baselines do not suppress automatic promoted-baseline lookup",
    )
    assert_true(
        'Use either --no-baseline or --compare-to, not both' in entrypoint,
        "bench entrypoint does not reject contradictory baseline policy",
    )


def test_importer_receives_hardware_scoring_identity_and_rejects_stale_baseline() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        out_dir = root / "out"
        out_dir.mkdir()
        csv_path = root / "perf.csv"
        csv_path.write_text("header\n", encoding="utf-8")
        scoring_fingerprint = "a" * 64
        identity = {
            "schema_version": 2,
            "kind": "bench_identity",
            "product_fingerprint": "b" * 64,
            "grader_fingerprint": "c" * 64,
            "hardware_scoring_fingerprint": scoring_fingerprint,
            "scenario_fingerprint": "d" * 64,
            "scenario": {"parameters": {"suite": "core"}},
        }
        baseline_root = root / "baselines"
        baseline_dir = run_window_module.baseline_directory(
            baseline_root,
            "release",
            identity,
        )
        baseline_dir.mkdir(parents=True)
        (baseline_dir / "manifest.json").write_text("{}\n", encoding="utf-8")
        stale_identity = dict(identity)
        stale_identity["hardware_scoring_fingerprint"] = "e" * 64
        (baseline_dir / "identity.json").write_text(
            json.dumps(stale_identity),
            encoding="utf-8",
        )
        args = SimpleNamespace(
            suite="core",
            board_id="release",
            git_sha="f" * 40,
            git_ref="main",
            profile="drive_wifi_off",
            segment="last",
            lane="bench",
            compare_to=[],
            baseline_root=str(baseline_root),
        )
        captured: list[str] = []
        original_run = run_window_module.subprocess.run

        def fake_run(cmd, **_kwargs):
            captured.extend(str(item) for item in cmd)
            return subprocess.CompletedProcess(cmd, 0, stdout="ok\n", stderr="")

        run_window_module.subprocess.run = fake_run
        try:
            run_window_module.run_import(args, csv_path, out_dir, identity)
        finally:
            run_window_module.subprocess.run = original_run

        flag_index = captured.index("--hardware-scoring-fingerprint")
        assert_true(
            captured[flag_index + 1] == scoring_fingerprint,
            f"importer did not receive exact scoring identity: {captured}",
        )
        assert_true(
            "--compare-to" not in captured,
            f"scoring-incompatible automatic baseline was admitted: {captured}",
        )

        (baseline_dir / "identity.json").write_text(
            json.dumps(identity),
            encoding="utf-8",
        )
        (baseline_dir / "manifest.json").write_text(
            "{malformed automatic baseline manifest\n",
            encoding="utf-8",
        )
        captured.clear()
        run_window_module.subprocess.run = fake_run
        try:
            run_window_module.run_import(args, csv_path, out_dir, identity)
        finally:
            run_window_module.subprocess.run = original_run
        assert_true(
            "--compare-to" not in captured,
            f"malformed optional automatic manifest was admitted: {captured}",
        )

        baseline_manifest = {
            "schema_version": 1,
            "run_id": "baseline",
            "timestamp_utc": "2026-08-15T00:00:00Z",
            "git_sha": "1" * 40,
            "git_ref": "main",
            "product_fingerprint": identity["product_fingerprint"],
            "grader_fingerprint": identity["grader_fingerprint"],
            "hardware_scoring_fingerprint": identity[
                "hardware_scoring_fingerprint"
            ],
            "scenario_fingerprint": identity["scenario_fingerprint"],
            "run_kind": "real_fw_soak",
            "board_id": "release",
            "env": "perf-csv-import",
            "lane": "bench-core",
            "suite_or_profile": "drive_wifi_off",
            "stress_class": "core",
            "result": "PASS",
            "metrics_file": "metrics.ndjson",
            "scoring_file": "scoring.json",
            "tracks": ["drive_wifi_off"],
            "source_type": "perf_csv",
            "source_schema": 46,
        }
        (baseline_dir / "manifest.json").write_text(
            json.dumps(baseline_manifest),
            encoding="utf-8",
        )
        metrics_path = baseline_dir / "metrics.ndjson"
        captured.clear()
        run_window_module.subprocess.run = fake_run
        try:
            run_window_module.run_import(args, csv_path, out_dir, identity)
        finally:
            run_window_module.subprocess.run = original_run
        assert_true(
            "--compare-to" not in captured,
            f"optional automatic baseline with missing metrics was admitted: {captured}",
        )

        metrics_path.write_text("{malformed baseline metric\n", encoding="utf-8")
        captured.clear()
        run_window_module.subprocess.run = fake_run
        try:
            run_window_module.run_import(args, csv_path, out_dir, identity)
        finally:
            run_window_module.subprocess.run = original_run
        assert_true(
            "--compare-to" not in captured,
            f"corrupt optional automatic metrics were admitted: {captured}",
        )

        metrics_path.write_text(
            json.dumps(
                {
                    "schema_version": 1,
                    "run_id": "baseline",
                    "git_sha": "1" * 40,
                    "run_kind": "real_fw_soak",
                    "suite_or_profile": "drive_wifi_off",
                    "metric": "sd_max_peak_us",
                    "sample": 0,
                    "value": 10000,
                    "unit": "us",
                    "tags": {},
                }
            )
            + "\n",
            encoding="utf-8",
        )
        captured.clear()
        run_window_module.subprocess.run = fake_run
        try:
            run_window_module.run_import(args, csv_path, out_dir, identity)
        finally:
            run_window_module.subprocess.run = original_run
        automatic_manifest = str(baseline_dir / "manifest.json")
        assert_true(
            automatic_manifest in captured,
            f"valid optional automatic baseline was rejected: {captured}",
        )

        explicit_manifest = root / "explicit-baseline.json"
        explicit_manifest.write_text("{malformed explicit baseline\n", encoding="utf-8")
        (baseline_dir / "identity.json").write_text(
            "{malformed automatic baseline identity\n",
            encoding="utf-8",
        )
        args.compare_to = [str(explicit_manifest)]
        captured.clear()
        run_window_module.subprocess.run = fake_run
        try:
            run_window_module.run_import(args, csv_path, out_dir, identity)
        finally:
            run_window_module.subprocess.run = original_run
        compare_indices = [
            index for index, value in enumerate(captured) if value == "--compare-to"
        ]
        assert_true(
            compare_indices == [len(captured) - 2]
            and captured[compare_indices[0] + 1] == str(explicit_manifest),
            f"explicit baseline was not forwarded unchanged: {captured}",
        )
        assert_true(
            str(baseline_dir / "manifest.json") not in captured,
            f"malformed optional automatic baseline was admitted: {captured}",
        )


def test_baseline_promotion_is_future_core_display_only() -> None:
    entrypoint = ROOT / "bench.sh"
    text = entrypoint.read_text(encoding="utf-8")
    assert_true(
        "Does not compare the current run" in text,
        "promotion help does not explain that scoring happens before promotion",
    )
    assert_true(
        "Current manifests retain the baseline comparison available when they were scored." in text,
        "promotion result does not preserve manifest provenance",
    )
    assert_true('"schema_version": 3' in text, "promoted baseline metadata schema is stale")
    assert_true(
        '"hardware_scoring_fingerprint": "$hardware_scoring_fingerprint"' in text,
        "promoted baseline does not retain hardware-scoring identity",
    )

    proc = subprocess.run(
        [str(entrypoint), "--replay", "--promote-baseline"],
        cwd=ROOT,
        text=True,
        capture_output=True,
        check=False,
    )
    assert_true(proc.returncode == 3, f"replay-only promotion was not rejected: {proc}")
    assert_true(
        "replay baselines are not promoted" in proc.stderr,
        f"replay-only promotion failure was unclear: {proc.stderr}",
    )


def test_camera_profile_validation_rejects_recorder_brightness_drift() -> None:
    preflight = bytes([3]) * FRAME_BYTES
    stable_video = bytes([4]) * FRAME_BYTES
    stable = evaluate_camera_profile_frames(preflight, stable_video)
    assert_true(stable["result"] == "PASS", f"stable camera profile failed: {stable}")

    drifted_video = bytearray(stable_video)
    x0, y0, x1, y1 = CALIBRATION_PATCH
    for y in range(y0, y1):
        for x in range(x0, x1):
            drifted_video[y * FRAME_WIDTH + x] = 60
    drifted = evaluate_camera_profile_frames(preflight, bytes(drifted_video))
    assert_true(drifted["result"] == "FAIL", f"bright recorder handoff passed: {drifted}")
    assert_true("recorder handoff" in drifted["message"], f"wrong profile failure: {drifted}")


def test_only_captured_replay_video_is_mechanically_graded() -> None:
    captured = {"result": "CAPTURED"}
    assert_true(not camera_grade_required("core", captured), "core camera became a verdict")
    assert_true(not camera_grade_required("display", captured), "display camera became a verdict")
    assert_true(camera_grade_required("replay", captured), "captured replay camera was not graded")
    assert_true(
        not camera_grade_required("replay", {"result": "CAPTURE_FAILED"}),
        "failed replay capture was sent to the visual grader",
    )


def camera_observation(
    time_s: float,
    *,
    alert: bool,
    frequency: int | None = None,
    direction: str = "UNKNOWN",
) -> FrameObservation:
    return FrameObservation(
        time_s=time_s,
        visible_pixels=100,
        frequency_pixels=100 if alert else 0,
        frequency_mhz=frequency,
        frequency_confidence=0.1 if frequency is not None else 0.0,
        frequency_signature=(),
        direction=direction,
        direction_confidence=1.0 if direction != "UNKNOWN" else 0.0,
    )


def matching_replay_fixture(
    offset: float,
) -> tuple[list[FrameObservation], list[EncounterObservation]]:
    observations: list[FrameObservation] = []
    for index in range(252 * 3):
        replay_time = index / 3
        active = (5 <= replay_time < 56) or (59 <= replay_time < 244)
        observations.append(
            camera_observation(
                offset + replay_time,
                alert=active,
                frequency=24150 if active else None,
                direction="FRONT" if active else "UNKNOWN",
            )
        )
    encounters = [
        EncounterObservation(
            time_s=float(second),
            encounter_id=1 if second <= 56 else 2,
            frequency_mhz=24150,
            direction="FRONT",
            event="SAMPLE",
        )
        for second in (*range(5, 57), *range(59, 245))
    ]
    return observations, encounters


def transition_boundary_replay_fixture() -> tuple[
    list[FrameObservation], list[EncounterObservation], float
]:
    offset = 7.215

    def state_at(replay_time: float) -> tuple[int, str]:
        if replay_time < 59:
            phase = max(0, int((replay_time - 5) // 4))
        else:
            phase = max(0, int((replay_time - 59) // 25))
        states = ((24150, "FRONT"), (34700, "SIDE"), (35500, "REAR"))
        return states[phase % len(states)]

    observations: list[FrameObservation] = []
    for index in range(920):
        video_time = index / 3
        replay_time = video_time - offset
        active = (5 <= replay_time < 56) or (59 <= replay_time < 244)
        frequency, direction = state_at(replay_time)
        observations.append(
            camera_observation(
                video_time,
                alert=active,
                frequency=frequency if active else None,
                direction=direction if active else "UNKNOWN",
            )
        )

    times = [float(second) for second in range(5, 57)]
    times.extend(float(second) for second in range(59, 245, 5))
    encounters: list[EncounterObservation] = []
    for time_s in times:
        frequency, direction = state_at(time_s)
        if time_s in {5.0, 59.0}:
            event = "START"
        elif time_s in {56.0, 244.0}:
            event = "END"
        else:
            event = "SAMPLE"
        encounters.append(
            EncounterObservation(
                time_s=time_s,
                encounter_id=1 if time_s <= 56 else 2,
                frequency_mhz=frequency,
                direction=direction,
                event=event,
            )
        )
    return observations, encounters, offset


def frequency_signature_fixture(frequency: int) -> tuple[int, ...]:
    patterns_by_digit = {
        digit: pattern for pattern, digit in camera_grade_module.SEGMENT_PATTERNS.items()
    }
    return tuple(
        value
        for digit in f"{frequency:05d}"
        for value in (1000 if active else 0 for active in patterns_by_digit[int(digit)])
    )


def test_reference_free_segment_decoder_abstains_when_ambiguous() -> None:
    for expected in (24150, 34700, 35500):
        signature = frequency_signature_fixture(expected)
        actual, confidence = identify_frequency(signature)
        assert_true(actual == expected, f"segment decoder read {actual} instead of {expected}")
        assert_true(confidence > 0.0, f"segment decoder had no confidence for {expected}")

    blank_signature = frequency_signature(bytes(camera_grade_module.FRAME_BYTES))
    assert_true(not any(blank_signature), "blank frame produced active frequency segments")

    ambiguous = list(frequency_signature_fixture(24150))
    ambiguous[0] = (
        camera_grade_module.SEGMENT_OFF_THRESHOLD
        + camera_grade_module.SEGMENT_ON_THRESHOLD
    ) // 2
    actual, confidence = identify_frequency(tuple(ambiguous))
    assert_true(actual is None and confidence == 0.0, "ambiguous segment was guessed")


def test_frequency_sampling_ignores_neighboring_stroke_bleed() -> None:
    frame = bytearray(camera_grade_module.FRAME_BYTES)

    def fill(bounds: tuple[int, int, int, int]) -> None:
        for y in range(bounds[1], bounds[3]):
            for x in range(bounds[0], bounds[2]):
                offset = (y * camera_grade_module.FRAME_WIDTH + x) * 3
                frame[offset : offset + 3] = bytes((255, 100, 0))

    # Bleed immediately outside the fifth digit's upper-right and middle
    # segment interiors must not turn an inactive segment into a vote.
    for bounds in ((299, 64, 307, 68), (299, 81, 307, 84), (281, 82, 283, 90), (301, 82, 303, 90)):
        fill(bounds)
    signature = frequency_signature(bytes(frame))
    assert_true(
        signature[29] <= camera_grade_module.SEGMENT_OFF_THRESHOLD,
        f"upper-right segment sampled neighboring bleed: {signature[29]}",
    )
    assert_true(
        signature[34] <= camera_grade_module.SEGMENT_OFF_THRESHOLD,
        f"middle segment sampled neighboring bleed: {signature[34]}",
    )


def registration_fixture(offset_x: float, offset_y: float) -> bytes:
    glyphs = (
        ("11111", "10000", "10000", "11111", "00001", "00001", "11111"),
        ("11111", "10000", "10000", "10000", "10000", "10000", "11111"),
        ("01110", "10001", "10001", "11111", "10001", "10001", "10001"),
        ("10001", "11001", "10101", "10011", "10001", "10001", "10001"),
    )
    cell_width = 5
    cell_height = 7
    gap_cells = 2
    landmark_width = (4 * 5 + 3 * gap_cells) * cell_width
    landmark_height = 7 * cell_height
    frame = bytearray(REGISTRATION_WIDTH * REGISTRATION_HEIGHT * 3)
    anchor_x = REGISTRATION_WIDTH * (
        DISPLAY_CROP_X + DISPLAY_CROP_WIDTH * (REFERENCE_ANCHOR_X + offset_x) / FRAME_WIDTH
    )
    anchor_y = REGISTRATION_HEIGHT * (
        DISPLAY_CROP_Y + DISPLAY_CROP_HEIGHT * (REFERENCE_ANCHOR_Y + offset_y) / FRAME_HEIGHT
    )
    x0 = round(anchor_x - (landmark_width - 1) / 2)
    y0 = round(anchor_y - (landmark_height - 1) / 2)
    cursor_cells = 0
    for glyph in glyphs:
        for cell_y, row in enumerate(glyph):
            for cell_x, active in enumerate(row):
                if active != "1":
                    continue
                pixel_x0 = x0 + (cursor_cells + cell_x) * cell_width
                pixel_y0 = y0 + cell_y * cell_height
                for y in range(pixel_y0, pixel_y0 + cell_height):
                    for x in range(pixel_x0, pixel_x0 + cell_width):
                        index = (y * REGISTRATION_WIDTH + x) * 3
                        frame[index : index + 3] = bytes((255, 100, 10))
        cursor_cells += 5 + gap_cells
    return bytes(frame)


def test_camera_crop_registration_tracks_dynamic_rig_movement() -> None:
    _offset_x, _offset_y, registration = detect_display_crop_registration(
        registration_fixture(64.0, 8.0)
    )
    assert_true(registration["result"] == "PASS", "bounded camera registration did not pass")
    assert_true(
        registration["transform"]["kind"] == "dynamic_similarity",
        f"camera did not record a dynamic transform: {registration}",
    )
    first_crop = registration["transform"]["crop_fractions"]
    _x, _y, moved = detect_display_crop_registration(registration_fixture(-60.0, 30.0))
    assert_true(
        moved["transform"]["crop_fractions"] != first_crop,
        "moving the DUT did not move the normalized crop",
    )

    try:
        detect_display_crop_registration(registration_fixture(350.0, 0.0))
    except camera_grade_module.CameraRegistrationError as exc:
        assert_true(
            exc.diagnostic["code"] == "screen_crop_outside_frame",
            f"unexpected registration error: {exc}",
        )
    else:
        raise AssertionError("out-of-frame dynamic crop passed registration")


def test_camera_crop_registration_falls_back_to_bright_still() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        camera_dir = Path(tmp)
        session_start = camera_dir / "session_start_exp50.jpg"
        bright = camera_dir / "final_auto.jpg"
        session_start.write_bytes(b"session")
        bright.write_bytes(b"bright")
        original = camera_grade_module.calibrate_display_crop
        calls: list[str] = []

        def fake_calibrate(path: Path) -> tuple[float, float, dict[str, object]]:
            calls.append(path.name)
            if path == session_start:
                raise RuntimeError("display transition")
            return 4.0, 2.0, {"result": "PASS", "source_still": path.name}

        camera_grade_module.calibrate_display_crop = fake_calibrate
        try:
            offset_x, offset_y, registration = (
                camera_grade_module.calibrate_display_crop_from_evidence(
                    camera_dir,
                    {
                        "session_start_still": session_start.name,
                        "bright_still": bright.name,
                    },
                )
            )
        finally:
            camera_grade_module.calibrate_display_crop = original

        assert_true(calls == [session_start.name, bright.name], f"wrong fallback order: {calls}")
        assert_true((offset_x, offset_y) == (4.0, 2.0), "bright-still registration was lost")
        assert_true(registration["source_field"] == "bright_still", "fallback source was not recorded")


def test_camera_grade_rejects_visual_state_that_disagrees_with_log() -> None:
    offset = 2.0
    observations, encounters = matching_replay_fixture(offset)
    passed = grade_replay(observations, encounters, offset)
    assert_true(passed["result"] == "PASS", f"matching camera/log evidence failed: {passed}")
    adjustment = passed["alignment"]["hint_adjustment_seconds"]
    assert_true(
        abs(adjustment) < MAX_REPLAY_ALIGNMENT_ADJUSTMENT_S,
        f"camera/log alignment escaped the timing hint: {passed}",
    )

    wrong = [
        FrameObservation(
            **{
                **item.__dict__,
                "frequency_mhz": 35500 if item.alert_visible else None,
                "direction": "REAR" if item.alert_visible else "UNKNOWN",
            }
        )
        for item in observations
    ]
    failed = grade_replay(wrong, encounters, offset)
    assert_true(failed["result"] == "FAIL", f"camera/log disagreement passed: {failed}")
    assert_true(
        failed["alignment"] == passed["alignment"],
        "frequency/direction answers changed alert/rest alignment",
    )

    residual_observations, residual_encounters = matching_replay_fixture(1.0)
    residual = grade_replay(residual_observations, residual_encounters, 2.0)
    assert_true(residual["result"] == "PASS", f"bounded -1s residual failed: {residual}")
    assert_true(
        residual["alignment"]["hint_adjustment_seconds"] == -1.0,
        f"host/video residual was not recorded: {residual}",
    )


def test_replay_alignment_requires_hint_and_rejects_boundary() -> None:
    observations, encounters = matching_replay_fixture(2.0)
    for invalid_hint in (None, float("nan"), float("inf")):
        alignment = find_replay_alignment(observations, encounters, invalid_hint)
        assert_true(alignment["result"] == "INCONCLUSIVE", f"invalid hint passed: {alignment}")
        assert_true(
            alignment["diagnostic"]["code"] == "timing_anchor_missing",
            f"wrong missing-hint diagnostic: {alignment}",
        )

    # Process launch precedes BLE transport readiness. The measured fresh-boot
    # delay remains valid while a larger clock error still reaches the bound.
    delayed_observations, delayed_encounters = matching_replay_fixture(13.0 / 3.0)
    delayed_offset, _ratio = find_replay_offset(delayed_observations, delayed_encounters, 2.0)
    delayed_adjustment = abs(delayed_offset - 2.0)
    assert_true(
        2.0 <= delayed_adjustment < MAX_REPLAY_ALIGNMENT_ADJUSTMENT_S,
        f"measured BLE readiness delay escaped the timing guard: {delayed_offset}",
    )

    # A four-second clock error pushes the best available candidate to the
    # positive edge of the deliberately bounded three-second search window.
    boundary_observations, boundary_encounters = matching_replay_fixture(6.0)
    boundary = find_replay_alignment(boundary_observations, boundary_encounters, 2.0)
    assert_true(boundary["result"] == "INCONCLUSIVE", f"boundary alignment passed: {boundary}")
    assert_true(
        boundary["diagnostic"]["code"] == "alignment_search_boundary",
        f"wrong boundary diagnostic: {boundary}",
    )

    flat = [camera_observation(index / 3, alert=False) for index in range(920)]
    ambiguous = find_replay_alignment(flat, encounters, 2.0)
    assert_true(ambiguous["result"] == "INCONCLUSIVE", f"flat alignment passed: {ambiguous}")
    assert_true(
        ambiguous["diagnostic"]["code"] == "alignment_ambiguous",
        f"flat alignment had wrong diagnostic: {ambiguous}",
    )


def test_replay_camera_abstains_for_unreadable_or_ambiguous_answers() -> None:
    observations, encounters = matching_replay_fixture(2.0)
    unreadable = [
        FrameObservation(
            **{
                **item.__dict__,
                "frequency_mhz": None,
                "frequency_confidence": 0.0,
                "direction": "UNKNOWN",
                "direction_confidence": 0.0,
            }
        )
        for item in observations
    ]
    unreadable_grade = grade_replay(unreadable, encounters, 2.0)
    assert_true(
        unreadable_grade["result"] == "INCONCLUSIVE",
        f"unreadable answers became a product failure: {unreadable_grade}",
    )
    unreadable_codes = {item["code"] for item in unreadable_grade["diagnostics"]}
    assert_true(
        {"frequency_observations_insufficient", "direction_observations_insufficient"}
        <= unreadable_codes,
        f"missing unreadable diagnostics: {unreadable_grade}",
    )

    frequencies = (24150, 34700, 35500)
    directions = ("FRONT", "SIDE", "REAR")
    contradictory = [
        FrameObservation(
            **{
                **item.__dict__,
                "frequency_mhz": frequencies[index % len(frequencies)] if item.alert_visible else None,
                "direction": directions[index % len(directions)] if item.alert_visible else "UNKNOWN",
            }
        )
        for index, item in enumerate(observations)
    ]
    contradictory_grade = grade_replay(contradictory, encounters, 2.0)
    assert_true(
        contradictory_grade["result"] == "INCONCLUSIVE",
        f"contradictory nearby frames searched for a favorable answer: {contradictory_grade}",
    )
    assert_true(
        any(item["code"] == "encounter_classification_ambiguous" for item in contradictory_grade["diagnostics"]),
        f"contradictory consensus diagnostic missing: {contradictory_grade}",
    )


def test_replay_semantic_consensus_counts_unreadable_samples() -> None:
    observations, encounters = matching_replay_fixture(2.0)
    high_coverage = grade_replay(observations, encounters, 2.0)
    assert_true(high_coverage["result"] == "PASS", f"high-coverage match failed: {high_coverage}")

    alert_index = 0
    sparse: list[FrameObservation] = []
    for item in observations:
        readable = item.alert_visible and alert_index % 5 == 0
        if item.alert_visible:
            alert_index += 1
        sparse.append(
            FrameObservation(
                **{
                    **item.__dict__,
                    "frequency_mhz": item.frequency_mhz if readable else None,
                    "frequency_confidence": item.frequency_confidence if readable else 0.0,
                    "direction": item.direction if readable else "UNKNOWN",
                    "direction_confidence": item.direction_confidence if readable else 0.0,
                }
            )
        )
    sparse_grade = grade_replay(sparse, encounters, 2.0)
    assert_true(
        sparse_grade["result"] == "INCONCLUSIVE",
        f"sparse favorable semantic readings qualified: {sparse_grade}",
    )
    assert_true(
        sparse_grade["alignment"] == high_coverage["alignment"],
        "semantic readability changed alert/rest alignment",
    )
    assert_true(
        any(
            item["code"] == "encounter_classification_ambiguous"
            for item in sparse_grade["diagnostics"]
        ),
        f"sparse semantic diagnostic missing: {sparse_grade}",
    )

    confidently_wrong = [
        FrameObservation(
            **{
                **item.__dict__,
                "frequency_mhz": 35500 if item.alert_visible else None,
                "direction": "REAR" if item.alert_visible else "UNKNOWN",
            }
        )
        for item in observations
    ]
    wrong_grade = grade_replay(confidently_wrong, encounters, 2.0)
    assert_true(wrong_grade["result"] == "FAIL", f"confident semantic mismatch abstained: {wrong_grade}")


def test_replay_consensus_grades_stable_windows_not_planned_transitions() -> None:
    observations, encounters, offset = transition_boundary_replay_fixture()
    grade = grade_replay(observations, encounters, offset + 1.0)
    assert_true(grade["result"] == "PASS", f"planned transition ties blocked stable evidence: {grade}")
    stable_gate = grade["confidence"]["gates"]["stable_encounter_windows"]
    assert_true(
        stable_gate["transition_rows_excluded"] >= 10,
        f"planned transitions were not separated from stable rows: {grade}",
    )
    assert_true(
        grade["confidence"]["gates"]["encounter_consensus"]["ambiguous"] == 0,
        f"stable windows remained ambiguous: {grade}",
    )
    for check in (
        "logged_alerts_visible",
        "logged_frequencies_visible",
        "logged_directions_visible",
    ):
        assert_true(grade["checks"][check]["result"] == "PASS", f"stable check failed: {grade}")


def test_idle_camera_grade_rejects_unlogged_alerts() -> None:
    idle = [camera_observation(float(second), alert=False) for second in range(300)]
    assert_true(grade_idle(idle, 0.0, 300.0)["result"] == "PASS", "visible idle display failed")
    unexpected = [camera_observation(float(second), alert=second > 20) for second in range(300)]
    result = grade_idle(unexpected, 0.0, 300.0)
    assert_true(result["result"] == "FAIL", f"unlogged camera alerts passed: {result}")


def test_post_upload_settle_is_interruptible_and_skippable() -> None:
    intervals: list[float] = []
    wait_for_post_upload_settle(3, sleep=intervals.append)
    assert_true(intervals == [1.0, 1.0, 1.0], f"settle interval was not split into short waits: {intervals}")

    intervals.clear()
    wait_for_post_upload_settle(0, sleep=intervals.append)
    assert_true(not intervals, f"zero-second settle should be skipped: {intervals}")


def test_csv_export_retries_busy_admission_then_preserves_grading() -> None:
    class FakeClock:
        def __init__(self) -> None:
            self.now = 0.0
            self.sleeps: list[float] = []

        def monotonic(self) -> float:
            return self.now

        def sleep(self, seconds: float) -> None:
            self.sleeps.append(seconds)
            self.now += seconds

    class FakeSerial:
        def __init__(self) -> None:
            self.commands: list[str] = []
            self.lines = [
                'QERR {"ok":false,"error":"perf_sd_busy_retry"}',
                'QFILE {"path":"/perf/perf_boot_1.csv","size":3}',
                "QCHUNK 0 612C62",
                'QEND {"bytes":3,"crc32":"2CD913DF"}',
            ]

        def write_command(self, command: str) -> None:
            self.commands.append(command)

        def read_protocol_line(self, _prefixes: tuple[str, ...], _timeout: float) -> str:
            return self.lines.pop(0)

    clock = FakeClock()
    original_time = run_window_module.time
    run_window_module.time = SimpleNamespace(monotonic=clock.monotonic, sleep=clock.sleep)
    try:
        with tempfile.TemporaryDirectory() as tmp:
            serial = FakeSerial()
            csv_path = run_window_module.download_csv(serial, Path(tmp), 1)
            assert_true(csv_path.read_bytes() == b"a,b", "retried export changed CSV bytes")
            assert_true(serial.commands == ["QGETCSV", "QGETCSV"], f"wrong retry commands: {serial.commands}")
            assert_true(clock.sleeps == [0.25], f"wrong busy retry delay: {clock.sleeps}")
    finally:
        run_window_module.time = original_time


def test_csv_export_busy_retry_is_bounded_and_fails_closed() -> None:
    class FakeClock:
        def __init__(self) -> None:
            self.now = 0.0

        def monotonic(self) -> float:
            return self.now

        def sleep(self, seconds: float) -> None:
            self.now += seconds

    class BusySerial:
        def __init__(self) -> None:
            self.commands: list[str] = []

        def write_command(self, command: str) -> None:
            self.commands.append(command)

        def read_protocol_line(self, _prefixes: tuple[str, ...], _timeout: float) -> str:
            return 'QERR {"ok":false,"error":"perf_sd_busy_retry"}'

    clock = FakeClock()
    serial = BusySerial()
    original_time = run_window_module.time
    run_window_module.time = SimpleNamespace(monotonic=clock.monotonic, sleep=clock.sleep)
    try:
        with tempfile.TemporaryDirectory() as tmp:
            try:
                run_window_module.download_csv(serial, Path(tmp), 1)
            except RuntimeError as exc:
                assert_true("after retrying perf_sd_busy_retry for 15s" in str(exc), f"wrong timeout: {exc}")
            else:
                raise AssertionError("perpetually busy CSV export passed")
    finally:
        run_window_module.time = original_time
    assert_true(clock.now == 15.0, f"busy retry exceeded its deadline: {clock.now}")
    assert_true(len(serial.commands) == 60, f"wrong bounded retry count: {len(serial.commands)}")


def test_csv_export_unrelated_qerr_fails_without_retry() -> None:
    class FakeSerial:
        def __init__(self) -> None:
            self.commands: list[str] = []

        def write_command(self, command: str) -> None:
            self.commands.append(command)

        def read_protocol_line(self, _prefixes: tuple[str, ...], _timeout: float) -> str:
            return 'QERR {"ok":false,"error":"export_size_unavailable"}'

    serial = FakeSerial()
    with tempfile.TemporaryDirectory() as tmp:
        try:
            run_window_module.download_csv(serial, Path(tmp), 1)
        except RuntimeError as exc:
            assert_true("export_size_unavailable" in str(exc), f"wrong unrelated QERR: {exc}")
        else:
            raise AssertionError("unrelated QGETCSV error was retried or accepted")
    assert_true(serial.commands == ["QGETCSV"], f"unrelated QERR was retried: {serial.commands}")


def test_encounter_csv_path_uses_perf_boot_identity() -> None:
    assert_true(
        encounter_csv_sd_path("/perf/perf_boot_61-cbab7c22.csv")
        == "/encounters/encounters_61-cbab7c22.csv",
        "tokenized encounter path did not follow the perf boot identity",
    )
    assert_true(
        encounter_csv_sd_path("/perf/perf_boot_61.csv") == "/encounters/encounters_61.csv",
        "legacy encounter path did not follow the perf boot identity",
    )
    for invalid in ("", "/perf/other.csv", "/perf/perf_boot_.csv", "/perf/perf_boot_1/extra.csv"):
        assert_true(encounter_csv_sd_path(invalid) == "", f"invalid perf path was accepted: {invalid}")


def test_v1replay_tracks_each_subscription_independently() -> None:
    source_dir = ROOT / "tools" / "v1replay" / "Sources" / "v1replay"
    session = (source_dir / "Session.swift").read_text()
    peripheral = (source_dir / "Peripheral.swift").read_text()
    assert_true("private struct Subscription: Hashable" in session, "subscription identity is missing")
    assert_true("let central: UUID" in session, "subscription does not identify its central")
    assert_true("let channel: SubscriptionChannel" in session, "subscription does not identify its channel")
    assert_true(
        "var subscriptions: Set<Subscription> = []" in session,
        "replay session does not retain independent subscriptions",
    )
    assert_true(
        "subscriptions.remove(Subscription(central: central, channel: channel))" in session,
        "unsubscribe does not remove only the matching subscription",
    )
    assert_true(
        "session.subscribe(central: central.identifier, channel: channel)" in peripheral,
        "CoreBluetooth subscribe does not update the session",
    )
    assert_true(
        "$0.session.unsubscribe(\n                    central: central.identifier,\n                    channel: channel\n                )" in peripheral,
        "CoreBluetooth unsubscribe does not update the session",
    )


def test_v1replay_player_uses_live_control_snapshot() -> None:
    source_dir = ROOT / "tools" / "v1replay" / "Sources" / "v1replay"
    player = (source_dir / "Player.swift").read_text()
    options = player.split("struct Options {", 1)[1].split("\n    }", 1)[0]

    assert_true("var mode:" not in options, "Player.Options duplicates session mode")
    assert_true("var volume:" not in options, "Player.Options duplicates session volume")
    assert_true(
        player.count("let control = peripheral.controlState") == 2,
        "idle and active Player paths do not each read live session control state",
    )
    assert_true(
        player.count("controlState: control") == 2,
        "idle and active Player paths do not pass their live control snapshot",
    )


def test_v1replay_handshake_only_path_sends_once_then_holds_quiet() -> None:
    """Public behavior ID: V1-RECONNECT-SESSION-001."""
    source_dir = ROOT / "tools" / "v1replay" / "Sources" / "v1replay"
    player = (source_dir / "Player.swift").read_text(encoding="utf-8")
    peripheral = (source_dir / "Peripheral.swift").read_text(encoding="utf-8")
    main_source = (source_dir / "main.swift").read_text(encoding="utf-8")
    method = player.split("private func runHandshakeOnly()", 1)[1].split(
        "private func emitIdle", 1
    )[0]
    ensure_method = player.split("func ensureHandshakeOnlyClear()", 1)[1].split(
        "func handshakeOnlyClearDelivered()", 1
    )[0]
    delivered_method = player.split("func handshakeOnlyClearDelivered()", 1)[1].split(
        "func toggleMuteOverride()", 1
    )[0]
    wait_method = player.split("private func waitForCentral()", 1)[1].split(
        "private func transportReady()", 1
    )[0]
    peripheral_ensure = peripheral.split("func ensureHandshakeClear", 1)[1].split(
        "private func appendPending", 1
    )[0]
    epoch_cleanup = peripheral.split("private func discardPendingHandshakeClear()", 1)[1].split(
        "private func endHandshakeEpoch()", 1
    )[0]
    epoch_end = peripheral.split("private func endHandshakeEpoch()", 1)[1].split(
        "private func send(_ decision", 1
    )[0]
    transport_reset = peripheral.split("private func resetSessionTransport()", 1)[1].split(
        "private func send(_ decision", 1
    )[0]
    stop_method = peripheral.split("func stop(onStopping:", 1)[1].split(
        "// MARK: - Command handling", 1
    )[0]
    adapter_state = peripheral.split("func peripheralManagerDidUpdateState", 1)[1].split(
        "func peripheralManager(_ peripheral: CBPeripheralManager,\n"
        "                           didAdd service", 1
    )[0]

    assert_true(
        "PlaybackPacketPlan.handshakeOnlyEmissions" in ensure_method
        and ensure_method.count("peripheral.ensureHandshakeClear(clear.bytes)") == 1,
        "handshake-only start does not ensure its one canonical clear plan",
    )
    assert_true(
        method.count("ensureHandshakeOnlyClear()") == 1
        and "send(emission)" not in method,
        "handshake-only polling fallback bypasses the shared clear ensure path",
    )
    assert_true(
        peripheral_ensure.count("self.appendPending(PendingNotification(") == 1
        and "case .retryPending:\n                break" in peripheral_ensure,
        "a pre-delivery start can append a duplicate canonical clear",
    )
    assert_true(
        "while !isStopped" in method
        and "sendIdleFrame" not in method
        and "playTimeline" not in method,
        "handshake-only enters an idle or encounter stream",
    )
    assert_true(
        player.count("Handshake-only ready —") == 1
        and "handshakeClearDeliveryConfirmed = true" in delivered_method,
        "handshake readiness is not emitted only after confirmed clear delivery",
    )
    assert_true(
        "options.handshakeOnly && handshakeClearDeliveryConfirmed" in wait_method,
        "an early delivered clear is overwritten when the Player polling thread starts",
    )
    start_callback = main_source.index("peripheral.onStartAlertData = {")
    delivery_callback = main_source.index("peripheral.onHandshakeClearDelivered = {")
    player_start = main_source.index("player.start()")
    assert_true(
        delivery_callback < start_callback < player_start
        and "player.ensureHandshakeOnlyClear()"
        in main_source[start_callback:player_start],
        "handshake-only start and delivery callbacks are not wired before polling starts",
    )
    update = peripheral.index("guard manager.updateValue(")
    dequeue = peripheral.index("pending.removeFirst()", update)
    ledger_delivery = peripheral.index("handshakeLedger?.recordDelivered(", dequeue)
    ready_callback = peripheral.index("onHandshakeClearDelivered?()", ledger_delivery)
    assert_true(
        update < dequeue < ledger_delivery < ready_callback,
        "handshake ready can precede CoreBluetooth acceptance or ledger delivery evidence",
    )
    accepted_request = peripheral.index("handshakeLedger?.recordAcceptedRequest(")
    start_hook = peripheral.index("onStartAlertData?()", accepted_request)
    assert_true(
        accepted_request < start_hook,
        "a post-delivery start can bypass the authoritative request ledger",
    )
    assert_true(
        "pending.removeAll { $0.purpose == .handshakeClear }" in epoch_cleanup
        and "discardPendingHandshakeClear()" in epoch_end
        and peripheral.count("handshakeLedger?.endEpoch()") == 1,
        "an ended evidence epoch can retain a stale handshake clear",
    )
    assert_true(
        "isStopping = true" in stop_method
        and "resetSessionTransport()" in stop_method
        and "guard !self.isStopping" in peripheral_ensure
        and "let characteristic = self.displayChar" in peripheral_ensure,
        "handshake clear work can cross teardown or use an off-queue characteristic",
    )
    assert_true(
        all(
            fragment in transport_reset
            for fragment in (
                "endHandshakeEpoch()",
                "pending.removeAll()",
                "lastValues.removeAll()",
                "shortSubscriberIDs.removeAll()",
                "$0.session.resetTransport()",
            )
        )
        and "guard peripheral.state == .poweredOn else" in adapter_state
        and "resetSessionTransport()" in adapter_state
        and stop_method.index("resetSessionTransport()") < stop_method.index("onStateChange?()"),
        "adapter loss or teardown can retain active session ownership evidence",
    )
    assert_true(
        'V1REPLAY_EVENT {\\"state\\":\\"handshake_transport\\"' in main_source
        and "peripheralConfig.handshakeLedger?.activeEpoch != nil" in main_source,
        "preflight readiness does not expose the current active ledger session",
    )

    runner = (ROOT / "scripts" / "bench" / "run_window.py").read_text(encoding="utf-8")
    preflight = runner.index("reconnect_preflight_result = run_reconnect_preflight(")
    camera = runner.index("admit_camera()", preflight)
    qstart = runner.index("completion = start_and_wait(", camera)
    assert_true(
        preflight < camera < qstart,
        "replay camera recording does not start after reconnect cleanup and before QSTART",
    )


def main() -> int:
    test_idle_emulator_covers_and_stops_with_window()
    test_failed_window_still_stops_emulator()
    test_idle_emulator_requires_current_process_transport_ownership()
    test_managed_stop_requires_graceful_stopped_marker()
    test_managed_stop_rejects_nonzero_exit_after_stopped_marker()
    test_all_managed_modes_require_current_stopping_ownership_snapshot()
    test_idle_completion_rejects_transport_loss_even_after_reownership()
    test_idle_completion_grades_loss_during_managed_stop()
    test_idle_shutdown_requires_prior_admission_and_strict_ordered_events()
    test_idle_admission_rejects_transport_already_lost()
    test_qstart_companion_failures_abort_the_active_dut_window()
    test_radio_lease_is_inherited_and_owner_pid_is_forwarded()
    test_first_signal_makes_cleanup_non_interruptible()
    test_live_cleanup_stops_emulators_before_serial_and_camera()
    test_live_cleanup_preserves_primary_failure_when_emulator_stop_also_fails()
    test_replay_requires_machine_completion_before_managed_stop()
    test_replay_blink_profile_argv_and_result()
    test_reconnect_preflight_ledger_requires_one_bounded_epoch()
    test_reconnect_preflight_ledger_accepts_only_bounded_timed_pre_stream_retries()
    test_reconnect_preflight_ledger_rejects_unverifiable_timing()
    test_reconnect_preflight_readiness_uses_delivery_ledger_not_console_order()
    test_reconnect_preflight_notification_hold_defaults_off_and_forwards_when_selected()
    test_reconnect_preflight_notification_hold_rejects_invalid_arguments()
    test_reconnect_preflight_observation_catches_late_invalid_ledger()
    test_reconnect_preflight_observation_rejects_invalid_arguments_before_start()
    test_reconnect_preflight_pre_stop_fence_timeout_is_narrow_and_evidence_owned()
    test_reconnect_preflight_pre_stop_fence_timeout_rejects_invalid_before_start()
    test_reconnect_preflight_propagates_interruption_without_terminalizing_emulator()
    test_reconnect_preflight_orders_fence_stop_cleanup_and_second_fence()
    test_reconnect_serial_fence_requires_safe_status_shape()
    test_reconnect_readiness_uses_unique_fifo_barrier_before_status_fence()
    test_reconnect_preflight_failure_retains_terminal_result()
    test_reconnect_preflight_distinguishes_behavior_from_broken_evidence()
    test_reconnect_preflight_process_uses_separate_quiet_artifacts()
    test_handshake_ledger_runner_and_delivery_wiring_are_pinned()
    test_global_shutter_default_uses_qualified_720p200_profile()
    test_native_camera_recorder_uses_host_clock_timeline()
    test_camera_failure_marker_aborts_active_window()
    test_live_camera_failure_is_serialized_as_evidence_failure()
    test_camera_stop_timeout_exceeds_native_finalize_timeout()
    test_run_logged_forwards_first_signal_received_during_startup()
    test_camera_probe_failure_retains_sanitized_diagnostics()
    test_early_recorder_exit_is_latched_as_capture_failure()
    test_unadmitted_camera_is_not_reported_as_capture_failure()
    test_reconnect_failure_before_camera_admission_leaves_no_camera_artifact()
    test_camera_grader_integrates_high_speed_frames_before_sampling()
    test_camera_profile_is_reapplied_after_recorder_open()
    test_camera_video_profile_seeds_exposure_before_aperture_priority()
    test_failed_frame_rate_probe_retains_measurements_and_diagnostics()
    test_bench_entrypoint_forwards_explicit_baseline_window()
    test_importer_receives_hardware_scoring_identity_and_rejects_stale_baseline()
    test_baseline_promotion_is_future_core_display_only()
    test_camera_profile_validation_rejects_recorder_brightness_drift()
    test_only_captured_replay_video_is_mechanically_graded()
    test_reference_free_segment_decoder_abstains_when_ambiguous()
    test_frequency_sampling_ignores_neighboring_stroke_bleed()
    test_camera_crop_registration_tracks_dynamic_rig_movement()
    test_camera_crop_registration_falls_back_to_bright_still()
    test_camera_grade_rejects_visual_state_that_disagrees_with_log()
    test_replay_alignment_requires_hint_and_rejects_boundary()
    test_replay_camera_abstains_for_unreadable_or_ambiguous_answers()
    test_replay_semantic_consensus_counts_unreadable_samples()
    test_replay_consensus_grades_stable_windows_not_planned_transitions()
    test_idle_camera_grade_rejects_unlogged_alerts()
    test_post_upload_settle_is_interruptible_and_skippable()
    test_csv_export_retries_busy_admission_then_preserves_grading()
    test_csv_export_busy_retry_is_bounded_and_fails_closed()
    test_csv_export_unrelated_qerr_fails_without_retry()
    test_encounter_csv_path_uses_perf_boot_identity()
    test_v1replay_tracks_each_subscription_independently()
    test_v1replay_player_uses_live_control_snapshot()
    test_v1replay_handshake_only_path_sends_once_then_holds_quiet()
    print("bench window tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
