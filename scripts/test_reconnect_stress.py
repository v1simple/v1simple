#!/usr/bin/env python3
"""Focused fake-runtime tests for the reconnect-only HIL stress runner."""

from __future__ import annotations

import hashlib
import json
import sys
import tempfile
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts" / "bench"))

import reconnect_stress as stress  # noqa: E402


def assert_true(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


class FakeSerial:
    def __init__(self, path: Path) -> None:
        self.path = path
        self.path.write_text("BOOT bootId=fake\n", encoding="utf-8")
        self.boot_marker_count = 1
        self.disconnect_cleanup_count = 0
        self.closed = False

    def emit(self, line: str) -> None:
        with self.path.open("a", encoding="utf-8") as handle:
            handle.write(line + "\n")

    def record_host_boundary(self, label: str) -> None:
        self.emit(f"HOST_BOUNDARY {label}")

    def close(self) -> None:
        self.closed = True


class FakeEmulator:
    def __init__(self, ordinal: int) -> None:
        self.ordinal = ordinal
        self.stopped = False

    def stop(self) -> None:
        self.stopped = True


class FakePreflightError(RuntimeError):
    classification = stress.COLLECTION_FAILED
    failure_kind = "evidence_or_transport"
    result: dict[str, Any] = {}


class FakeRuntime:
    def __init__(
        self,
        modes: dict[int, str] | None = None,
        *,
        terminal_identity_drift: bool = False,
        binary_drift_cycle: int | None = None,
        upload_failure: bool = False,
        lease_failure: bool = False,
    ) -> None:
        self.modes = modes or {}
        self.terminal_identity_drift = terminal_identity_drift
        self.binary_drift_cycle = binary_drift_cycle
        self.upload_failure = upload_failure
        self.lease_failure = lease_failure
        self.identity_calls = 0
        self.sleeps: list[float] = []
        self.readiness_calls = 0
        self.emulators: dict[int, FakeEmulator] = {}
        self.serial: FakeSerial | None = None
        self.clock = 0.0
        self.binary_path: Path | None = None
        self.serial_grader = stress.ProductionRuntime()
        self.pre_stop_fence_timeouts: list[float] = []
        self.host_events: list[str] = []
        self.emulator_lease_fds: list[int] = []

    def now_utc(self) -> str:
        return "2026-08-14T12:00:00Z"

    def monotonic(self) -> float:
        self.clock += 0.01
        return self.clock

    def sleep(self, seconds: float) -> None:
        self.sleeps.append(seconds)
        self.clock += seconds
        if (
            self.binary_drift_cycle == len(self.sleeps)
            and self.binary_path is not None
        ):
            with self.binary_path.open("ab") as handle:
                handle.write(b"drift")

    def build_identity(self, config: stress.StressConfig) -> dict[str, Any]:
        self.identity_calls += 1
        product = "1" * 64
        if self.terminal_identity_drift and self.identity_calls >= 3:
            product = "9" * 64
        return {
            "schema_version": 1,
            "kind": "fake_identity",
            "git_sha": "a" * 40,
            "git_ref": "test",
            "git_worktree_clean": True,
            "product_fingerprint": product,
            "grader_fingerprint": "2" * 64,
            "stress_config_fingerprint": "3" * 64,
            "stress_contract": config.contract_parameters(),
        }

    def build_v1replay(self, path: Path) -> Path:
        path.write_text("fake build\n", encoding="utf-8")
        self.binary_path = path.parent / "fake_v1replay"
        self.binary_path.write_bytes(b"fake executable\n")
        self.binary_path.chmod(0o755)
        return self.binary_path

    def wait_for_port(self, preferred: str, timeout_seconds: int = 30) -> str:
        del preferred, timeout_seconds
        self.host_events.append("wait_for_port")
        return "/dev/fake"

    def upload_firmware(self, port: str, path: Path) -> None:
        del port
        self.host_events.append("upload")
        path.write_text("fake upload\n", encoding="utf-8")
        if self.upload_failure:
            raise RuntimeError("fake upload failed")

    def open_serial(self, port: str, baud: int, path: Path) -> FakeSerial:
        del port, baud
        self.host_events.append("open_serial")
        self.serial = FakeSerial(path)
        return self.serial

    def wait_ready(self, serial_session: FakeSerial, timeout_seconds: float) -> None:
        del timeout_seconds
        serial_session.emit("QSTATUS ready nonce=initial")

    def establish_readiness(
        self,
        serial_session: FakeSerial,
        timeout_seconds: float,
    ) -> None:
        del timeout_seconds
        self.readiness_calls += 1
        nonce = f"{self.readiness_calls:032x}"
        serial_session.emit(f">>> QBSC08 {nonce}")
        serial_session.emit(
            "QBSC08 "
            + json.dumps({"schema": 1, "nonce": nonce, "status": "ready"})
        )
        self._emit_status_fence(serial_session)

    @staticmethod
    def _emit_status_fence(serial_session: FakeSerial) -> None:
        serial_session.emit(">>> QSTATUS")
        serial_session.emit(
            'QRESP {"ok":true,"state":"idle","suite":"core","mode":"v1"}'
        )

    def final_fence(self, serial_session: FakeSerial, timeout_seconds: float) -> None:
        del timeout_seconds
        serial_session.emit("QSTATUS fence=terminal")

    def radio_lease(self) -> object:
        runtime = self

        class FakeRadioLease:
            fd = 73

            def __enter__(self) -> object:
                runtime.host_events.append("lease_acquire")
                if runtime.lease_failure:
                    raise RuntimeError("fake radio lease unavailable")
                return self

            def __exit__(self, _exc_type: object, _exc: object, _traceback: object) -> None:
                runtime.host_events.append("lease_release")

        return FakeRadioLease()

    def make_emulator(
        self,
        executable: Path,
        cycle_dir: Path,
        lease_fd: int,
    ) -> FakeEmulator:
        del executable
        ordinal = int(cycle_dir.name)
        self.host_events.append(f"emulator_{ordinal}")
        self.emulator_lease_fds.append(lease_fd)
        emulator = FakeEmulator(ordinal)
        self.emulators[ordinal] = emulator
        return emulator

    def run_preflight(
        self,
        serial_session: FakeSerial,
        emulator: FakeEmulator,
        timeout_seconds: float,
        *,
        pre_stop_fence_timeout_s: float,
    ) -> dict[str, Any]:
        del timeout_seconds
        self.pre_stop_fence_timeouts.append(pre_stop_fence_timeout_s)
        cycle_dir = (
            serial_session.path.parent / "cycles" / f"{emulator.ordinal:04d}"
        )
        serial_session.emit("HOST_BOUNDARY reconnect_preflight_start")
        serial_session.emit("HOST_BOUNDARY reconnect_preflight_fence_begin")
        self._emit_status_fence(serial_session)
        serial_session.emit("HOST_BOUNDARY reconnect_preflight_fence_complete")
        (cycle_dir / stress.LEDGER_NAME).write_text(
            '{"schema_version":2,"kind":"v1replay_handshake_ledger",'
            '"timebase":"epoch_monotonic_ms"}\n',
            encoding="utf-8",
        )
        (cycle_dir / stress.EMULATOR_LOG_NAME).write_text(
            'V1REPLAY_EVENT {"state":"configured"}\n'
            'V1REPLAY_EVENT {"state":"handshake_ready"}\n',
            encoding="utf-8",
        )
        serial_session.emit(f"cycle={emulator.ordinal} handshake")
        serial_session.emit("HOST_BOUNDARY reconnect_preflight_process_exited")
        serial_session.disconnect_cleanup_count += 1
        serial_session.emit("[BLE] V1 disconnected; cleared LCD BLE state at fake")
        serial_session.emit("HOST_BOUNDARY reconnect_post_cleanup_fence_begin")
        self._emit_status_fence(serial_session)
        serial_session.emit("HOST_BOUNDARY reconnect_post_cleanup_fence_complete")
        if self.modes.get(emulator.ordinal) == "interrupted":
            raise stress.StressInterrupted("fake signal")
        if self.modes.get(emulator.ordinal) == "host_failure":
            raise FakePreflightError("host transport failed after evidence collection")
        return {
            "handshake_ready_while_alive": True,
            "serial_fence_observed": True,
            "managed_stop": True,
            "graceful_stop_confirmed": True,
            "confirmed_exit": True,
            "returncode": 0,
            "serial_session_continuous": True,
            "boot_observed_before_second_complete": False,
            "cleanup_marker_count": 1,
        }

    def _assert_stopped(self, path: Path) -> int:
        ordinal = int(path.parent.name)
        assert_true(self.emulators[ordinal].stopped, "graded before emulator exit")
        return ordinal

    def grade_ledger(self, path: Path) -> dict[str, Any]:
        ordinal = self._assert_stopped(path)
        mode = self.modes.get(ordinal, "pass")
        if mode == "incomplete_ledger":
            return {
                "result": stress.COLLECTION_FAILED,
                "evidence": ["ledger ended before final delivery"],
            }
        count = 1 if mode == "one_start" else 3 if mode == "too_many" else 2
        result = stress.FAIL if mode in {"late_start", "host_failure"} else stress.PASS
        elapsed = [100] if count == 1 else [100, 1100, 2100][:count]
        return {
            "result": result,
            "evidence": (
                ["replay handshake start retry occurred after stream delivery"]
                if mode == "late_start"
                else ["secondary ledger did not complete"]
                if mode == "host_failure"
                else []
            ),
            "start_request_counts": [{"epoch": 1, "count": count}],
            "start_elapsed_ms": elapsed,
            "start_gap_ms": elapsed[1] - elapsed[0] if count == 2 else None,
        }

    def grade_emulator_log(self, path: Path) -> dict[str, Any]:
        ordinal = self._assert_stopped(path)
        if self.modes.get(ordinal) == "incomplete_log":
            return {
                "result": stress.COLLECTION_FAILED,
                "evidence": ["emulator log ended with a partial event"],
            }
        return {"result": stress.PASS, "evidence": [], "diagnostics": []}

    def grade_lifecycle(self, raw: Any, failure_kind: str = "") -> dict[str, Any]:
        if failure_kind == "evidence_or_transport":
            return {
                "result": stress.FAIL,
                "evidence": ["secondary lifecycle booleans are incomplete"],
            }
        del raw
        return {"result": stress.PASS, "evidence": []}

    def grade_serial_slice(self, path: Path, ordinal: int) -> dict[str, Any]:
        self._assert_stopped(path)
        if self.modes.get(ordinal) == "serial_mutation":
            text = path.read_text(encoding="utf-8")
            path.write_text(
                text.replace(
                    "[BLE] V1 disconnected; cleared LCD BLE state at fake\n",
                    "",
                ),
                encoding="utf-8",
            )
        return self.serial_grader.grade_serial_slice(path, ordinal)


def config(
    out_dir: Path,
    cycles: int = 3,
    *,
    upload: bool = False,
) -> stress.StressConfig:
    return stress.StressConfig(cycles=cycles, upload=upload, out_dir=out_dir)


def load_json(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def verify_terminal_hashes(root: Path) -> None:
    manifest = load_json(root / stress.RESULT_NAME)
    for entry in manifest["artifacts"]:
        path = root / entry["path"]
        actual = hashlib.sha256(path.read_bytes()).hexdigest()
        assert_true(actual == entry["sha256"], f"bad hash for {entry['path']}")
        assert_true(path.stat().st_size == entry["size_bytes"], "bad artifact size")


def test_second_start_release_contract() -> None:
    with tempfile.TemporaryDirectory() as temporary:
        selected = config(Path(temporary) / "stress").contract_parameters()
        assert_true(
            selected["handshake_clear_release_trigger"]
            == "second_accepted_start",
            "stress contract does not name the deterministic release trigger",
        )
        assert_true(
            selected["handshake_clear_release_safety_deadline_ms"] == 1999,
            "stress safety deadline no longer precedes the third retry slot",
        )
        assert_true(
            selected["post_ready_observation_ms"] == 1100,
            "stress observation window cannot expose a third one-second retry",
        )
        assert_true(
            selected["pre_stop_fence_timeout_ms"] == 250,
            "stress pre-stop fence extends the live post-delivery window",
        )
        assert_true(
            "handshake_notification_hold_ms" not in selected,
            "stress contract still describes a fixed notification hold",
        )

    runtime = stress.ProductionRuntime()
    captured: dict[str, Any] = {}
    original_emulator = runtime.run_window.V1Emulator

    def recording_emulator(*args: Any, **kwargs: Any) -> object:
        captured.update(kwargs)
        return object()

    runtime.run_window.V1Emulator = recording_emulator
    try:
        runtime.make_emulator(
            Path("/fake/v1replay"),
            Path("/fake/0001"),
            lease_fd=42,
        )
    finally:
        runtime.run_window.V1Emulator = original_emulator
    assert_true(captured.get("handshake_only") is True, "stress emulator is not handshake-only")
    assert_true(
        captured.get("handshake_notification_hold_ms") == 1999,
        "stress runner did not pass the bounded second-START release deadline",
    )
    assert_true(captured.get("lease_fd") == 42, "stress child did not inherit the radio lease")


def test_production_ledger_scorer_exposes_retry_timing() -> None:
    runtime = stress.ProductionRuntime()
    frames = runtime.run_window
    events = [
        {"event": "subscribe", "epoch": 1, "channel": "B2CE", "elapsed_ms": 0},
        *(
            {
                "event": "request",
                "epoch": 1,
                "channel": "B6D4",
                "bytes": frames.START_ALERT_REQUEST,
                "elapsed_ms": elapsed_ms,
            }
            for elapsed_ms in (100, 1100)
        ),
        {
            "event": "stream_started",
            "epoch": 1,
            "channel": "B2CE",
            "bytes": frames.EMPTY_ALERT_ROW,
            "delivery": "delivered",
            "elapsed_ms": 1150,
        },
        {
            "event": "request",
            "epoch": 1,
            "channel": "B6D4",
            "bytes": frames.VERSION_REQUEST,
            "elapsed_ms": 1200,
        },
        {
            "event": "response",
            "epoch": 1,
            "channel": "B2CE",
            "bytes": frames.VERSION_RESPONSE,
            "delivery": "delivered",
            "elapsed_ms": 1250,
        },
        {
            "event": "request",
            "epoch": 1,
            "channel": "B6D4",
            "bytes": frames.ALL_VOLUME_REQUEST,
            "elapsed_ms": 1300,
        },
        {
            "event": "response",
            "epoch": 1,
            "channel": "B2CE",
            "bytes": frames.ALL_VOLUME_RESPONSE,
            "delivery": "delivered",
            "elapsed_ms": 1350,
        },
    ]
    header = {
        "schema_version": 2,
        "kind": "v1replay_handshake_ledger",
        "timebase": "epoch_monotonic_ms",
    }
    with tempfile.TemporaryDirectory() as temporary:
        path = Path(temporary) / stress.LEDGER_NAME
        def score(selected: list[dict[str, Any]]) -> dict[str, Any]:
            path.write_text(
                "".join(json.dumps(record) + "\n" for record in [header, *selected]),
                encoding="utf-8",
            )
            return runtime.grade_ledger(path)

        result = score(events)
        missing_clear = score(events[:3])
        missing_start = score(events[:1])
        missing_followup = score(events[:4])
    assert_true(result["result"] == stress.PASS, f"production score failed: {result}")
    assert_true(result["start_elapsed_ms"] == [100, 1100], "elapsed_ms lost")
    assert_true(result["start_gap_ms"] == 1000, "retry gap lost")
    assert_true(
        missing_clear["delivery_obligations"]["missing_emulator_delivery"]
        == ["canonical_clear_row"],
        "accepted START without clear was not assigned to the emulator",
    )
    assert_true(
        missing_start["delivery_obligations"]["missing_firmware_request"]
        == ["start_request"],
        "missing START was not assigned to firmware",
    )
    assert_true(
        missing_followup["delivery_obligations"]["missing_firmware_request"]
        == ["version_request"],
        "missing post-clear request was not assigned to firmware",
    )
    timeout = {
        "classification": stress.FAIL,
        "failure_kind": "handshake_timeout",
        "message": "handshake timed out",
    }
    supporting = {"result": stress.PASS, "evidence": []}
    missing_clear_result = stress._classify_cycle(
        missing_clear,
        supporting,
        supporting,
        supporting,
        timeout,
        [],
    )[0]
    assert_true(
        missing_clear_result == stress.COLLECTION_FAILED,
        "123935 missing-clear shape was misclassified as firmware behavior",
    )
    for label, checks in (
        ("missing start", missing_start),
        ("missing follow-up", missing_followup),
    ):
        classified = stress._classify_cycle(
            checks,
            supporting,
            supporting,
            supporting,
            timeout,
            [],
        )[0]
        assert_true(classified == stress.FAIL, f"{label} was not firmware behavior")


def test_three_pass_cycles() -> None:
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary) / "stress"
        runtime = FakeRuntime()
        snapshots: list[dict[str, Any]] = []
        original_atomic_write = stress._atomic_write_json

        def recording_atomic_write(path: Path, payload: dict[str, Any]) -> None:
            if path.name == stress.RESULT_NAME:
                snapshots.append(json.loads(json.dumps(payload)))
            original_atomic_write(path, payload)

        stress._atomic_write_json = recording_atomic_write
        try:
            exit_code = stress.run_stress(config(root, upload=True), runtime)
        finally:
            stress._atomic_write_json = original_atomic_write
        terminal = load_json(root / stress.RESULT_NAME)
        assert_true(exit_code == 0 and terminal["result"] == stress.PASS, "3-pass run failed")
        assert_true(terminal["passed_cycles"] == 3, "wrong pass count")
        assert_true(runtime.sleeps == [2.1, 1.1, 1.1], "wrong pre-cycle waits")
        assert_true(
            runtime.host_events
            == [
                "lease_acquire",
                "wait_for_port",
                "upload",
                "wait_for_port",
                "open_serial",
                "emulator_1",
                "emulator_2",
                "emulator_3",
                "lease_release",
            ],
            f"radio lease did not enclose hardware/emulator ownership: {runtime.host_events}",
        )
        assert_true(
            runtime.emulator_lease_fds == [73, 73, 73],
            f"stress children did not share one inherited lease: {runtime.emulator_lease_fds}",
        )
        assert_true(runtime.readiness_calls == 4, "initial/per-cycle readiness barriers missing")
        assert_true(
            runtime.pre_stop_fence_timeouts == [0.25, 0.25, 0.25],
            "pre-stop fence is no longer bounded after the observation window",
        )
        assert_true(
            [
                item["attempted_cycles"]
                for item in snapshots
                if item.get("state") == "running"
            ]
            == [0, 1, 2, 3],
            "running manifest did not preserve per-cycle progress",
        )
        assert_true(
            [item["result"] for item in terminal["identity_checks"]]
            == [stress.PASS, stress.PASS, stress.PASS],
            "identity was not checked at all three stages",
        )
        assert_true(terminal["qualification"]["qualifying"] is True, "uploaded pass did not qualify")
        assert_true(
            len(terminal["v1replay_executable_checks"]) == 5
            and all(item["result"] == stress.PASS for item in terminal["v1replay_executable_checks"]),
            "executable was not checked after build, before every cycle, and at terminal",
        )
        progress = [
            json.loads(line)
            for line in (root / stress.PROGRESS_NAME).read_text(encoding="utf-8").splitlines()
        ]
        assert_true(len(progress) == 3, "progress is not append-only per cycle")
        assert_true(
            all(item["start_timing"] == {"start_elapsed_ms": [100, 1100], "start_gap_ms": 1000} for item in progress),
            "strict timing evidence missing from PASS progress",
        )
        for ordinal in range(1, 4):
            cycle_dir = root / "cycles" / f"{ordinal:04d}"
            assert_true((cycle_dir / "cycle_result.json").is_file(), "cycle result missing")
            assert_true((cycle_dir / stress.SERIAL_SLICE_NAME).is_file(), "serial slice missing")
        serial_text = (root / stress.SERIAL_LOG_NAME).read_text(encoding="utf-8").lower()
        assert_true("qstart" not in serial_text and "qgetcsv" not in serial_text, "metrics command leaked")
        assert_true("camera" not in serial_text, "camera action leaked")
        verify_terminal_hashes(root)


def test_fail_fast_at_cycle_two_preserves_evidence() -> None:
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary) / "stress"
        exit_code = stress.run_stress(config(root), FakeRuntime({2: "too_many"}))
        terminal = load_json(root / stress.RESULT_NAME)
        assert_true(exit_code == 2 and terminal["result"] == stress.FAIL, "behavior did not fail")
        assert_true(terminal["attempted_cycles"] == 2, "runner did not fail fast")
        assert_true(not (root / "cycles" / "0003").exists(), "cycle 3 ran after failure")
        assert_true((root / "cycles" / "0001" / "cycle_result.json").is_file(), "prior evidence lost")
        verify_terminal_hashes(root)


def test_incomplete_and_late_evidence_classification() -> None:
    cases = (
        ("one_start", 3, stress.COLLECTION_FAILED),
        ("incomplete_ledger", 3, stress.COLLECTION_FAILED),
        ("incomplete_log", 3, stress.COLLECTION_FAILED),
        ("host_failure", 3, stress.COLLECTION_FAILED),
        ("serial_mutation", 3, stress.COLLECTION_FAILED),
        ("late_start", 2, stress.FAIL),
    )
    for mode, expected_exit, expected_result in cases:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / mode
            exit_code = stress.run_stress(config(root, 1), FakeRuntime({1: mode}))
            terminal = load_json(root / stress.RESULT_NAME)
            assert_true(exit_code == expected_exit, f"{mode}: wrong exit")
            assert_true(terminal["result"] == expected_result, f"{mode}: wrong result")


def test_reuse_invalid_arguments_and_terminal_drift() -> None:
    assert_true(
        isinstance(stress.StressInterrupted("signal"), InterruptedError),
        "signal marker does not use run_reconnect_preflight's interruption escape path",
    )
    with tempfile.TemporaryDirectory() as temporary:
        parent = Path(temporary)
        root = parent / "stress"
        assert_true(stress.run_stress(config(root, 1), FakeRuntime()) == 0, "setup run failed")
        terminal = load_json(root / stress.RESULT_NAME)
        assert_true(
            terminal["qualification"]
            == {
                "qualifying": False,
                "flash_provenance": "unverified_existing_board",
                "reason": "--no-upload cannot bind the board's installed firmware to the recorded source identity.",
            },
            "--no-upload was not marked non-qualifying",
        )
        assert_true(
            terminal["board_firmware_identity"]["result"] == "N/A"
            and "must not be attributed" in terminal["board_firmware_identity"]["reason"],
            "--no-upload attributed host identity to unverified board firmware",
        )
        assert_true(
            "qualification: NON-QUALIFYING"
            in (root / stress.SUMMARY_NAME).read_text(encoding="utf-8"),
            "summary hid unverified flash provenance",
        )
        before = (root / stress.RESULT_NAME).read_bytes()
        try:
            stress.run_stress(config(root, 1), FakeRuntime())
            raise AssertionError("existing artifact directory was reused")
        except FileExistsError:
            pass
        assert_true((root / stress.RESULT_NAME).read_bytes() == before, "reuse modified evidence")

        for invalid in (0, stress.MAX_CYCLES + 1):
            invalid_root = parent / f"invalid-{invalid}"
            try:
                stress.run_stress(config(invalid_root, invalid), FakeRuntime())
                raise AssertionError("invalid cycle count accepted")
            except ValueError:
                pass
            assert_true(not invalid_root.exists(), "invalid run created artifacts")

        drift_root = parent / "drift"
        exit_code = stress.run_stress(
            config(drift_root, 1),
            FakeRuntime(terminal_identity_drift=True),
        )
        terminal = load_json(drift_root / stress.RESULT_NAME)
        assert_true(exit_code == 3 and terminal["result"] == stress.COLLECTION_FAILED, "identity drift passed")
        assert_true(terminal["failure_kind"] == "identity_drift", "drift not classified")

        binary_root = parent / "binary-drift"
        exit_code = stress.run_stress(
            config(binary_root, 3),
            FakeRuntime(binary_drift_cycle=2),
        )
        terminal = load_json(binary_root / stress.RESULT_NAME)
        assert_true(exit_code == 3, "changed v1replay executable passed")
        assert_true(terminal["attempted_cycles"] == 1, "binary drift did not fail before cycle 2")
        assert_true(terminal["failure_kind"] == "executable_drift", "binary drift not classified")

        upload_root = parent / "upload-failure"
        exit_code = stress.run_stress(
            config(upload_root, 1, upload=True),
            FakeRuntime(upload_failure=True),
        )
        terminal = load_json(upload_root / stress.RESULT_NAME)
        assert_true(exit_code == 3, "failed upload passed")
        assert_true(
            terminal["qualification"]["flash_provenance"] == "upload_not_completed"
            and terminal["board_firmware_identity"]["result"] == "N/A",
            "failed upload was falsely attributed to the board",
        )

        interrupted_root = parent / "interrupted"
        exit_code = stress.run_stress(
            config(interrupted_root, 1),
            FakeRuntime({1: "interrupted"}),
        )
        terminal = load_json(interrupted_root / stress.RESULT_NAME)
        assert_true(exit_code == 130, "operator interruption did not return 130")
        assert_true(terminal["failure_kind"] == "interrupted", "interruption taxonomy changed")

        lease_root = parent / "lease-failure"
        lease_runtime = FakeRuntime(lease_failure=True)
        exit_code = stress.run_stress(config(lease_root, 1, upload=True), lease_runtime)
        terminal = load_json(lease_root / stress.RESULT_NAME)
        assert_true(
            exit_code == 3 and terminal["result"] == stress.COLLECTION_FAILED,
            "unavailable stress radio lease did not fail closed",
        )
        assert_true(
            lease_runtime.host_events == ["lease_acquire"],
            f"hardware was touched before lease admission: {lease_runtime.host_events}",
        )

    for argv in (
        ["--cycles", "1"],
        ["--cycles", "1", "--no-upload", "--camera"],
    ):
        try:
            stress.parse_args(argv)
            raise AssertionError(f"invalid CLI accepted: {argv}")
        except SystemExit as exc:
            assert_true(exc.code == 2, "invalid CLI returned wrong exit")


def main() -> int:
    test_second_start_release_contract()
    test_production_ledger_scorer_exposes_retry_timing()
    test_three_pass_cycles()
    test_fail_fast_at_cycle_two_preserves_evidence()
    test_incomplete_and_late_evidence_classification()
    test_reuse_invalid_arguments_and_terminal_drift()
    print("reconnect stress regression tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
