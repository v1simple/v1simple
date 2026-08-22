#!/usr/bin/env python3
"""Focused behavioral tests for bench host evidence collection."""

from __future__ import annotations

import hashlib
import json
import os
import sys
import tempfile
from pathlib import Path
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts" / "bench"))

import run_window  # noqa: E402
from camera_capture import CameraCapture  # noqa: E402


class FakeSerialPort:
    def __init__(self, responses: list[bytes]):
        self.responses = responses
        self.writes: list[bytes] = []
        self.is_open = False

    def open(self) -> None:
        self.is_open = True

    def reset_input_buffer(self) -> None:
        pass

    def write(self, payload: bytes) -> None:
        self.writes.append(payload)

    def flush(self) -> None:
        pass

    def readline(self) -> bytes:
        return self.responses.pop(0) if self.responses else b""

    def close(self) -> None:
        self.is_open = False


def read_ndjson(path: Path) -> list[dict[str, object]]:
    return [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()]


def test_serial_timeline_and_status_round_trips() -> None:
    response = b'QRESP {"ok":true,"state":"idle"}\n'
    port = FakeSerialPort([response, response])
    original_serial = run_window.serial
    try:
        run_window.serial = SimpleNamespace(Serial=lambda: port)
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            timeline = run_window.BenchTimeline(root / "bench_timeline.ndjson")
            serial = run_window.BenchSerial("fake", 115200, root / "bench_serial.log", timeline)
            try:
                assert run_window.capture_qstatus_round_trip(serial, "pre_window")["ok"] is True
                assert run_window.capture_qstatus_round_trip(serial, "post_window")["ok"] is True
            finally:
                serial.close()
                timeline.close()

            events = read_ndjson(timeline.path)
            assert port.writes == [b"QSTATUS\n", b"QSTATUS\n"]
            assert [event["event"] for event in events] == [
                "timeline_opened",
                "serial_send",
                "serial_receive",
                "qstatus_round_trip",
                "serial_send",
                "serial_receive",
                "qstatus_round_trip",
                "timeline_closed",
            ]
            round_trips = [event for event in events if event["event"] == "qstatus_round_trip"]
            assert [event["phase"] for event in round_trips] == ["pre_window", "post_window"]
            for event in round_trips:
                assert event["status"] == "observed"
                assert event["send_host_monotonic_ns"] <= event["receive_host_monotonic_ns"]
                assert event["duration_ns"] >= 0
    finally:
        run_window.serial = original_serial


def test_serial_buffers_partial_lines_until_newline() -> None:
    port = FakeSerialPort([b'QRESP {"ok":true,', b'"state":"idle"}\n'])
    original_serial = run_window.serial
    try:
        run_window.serial = SimpleNamespace(Serial=lambda: port)
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            timeline = run_window.BenchTimeline(root / "bench_timeline.ndjson")
            serial = run_window.BenchSerial("fake", 115200, root / "bench_serial.log", timeline)
            try:
                result = run_window.capture_qstatus_round_trip(serial, "partial_line")
            finally:
                serial.close()
                timeline.close()
            receives = [
                event for event in read_ndjson(timeline.path) if event["event"] == "serial_receive"
            ]
            assert result["ok"] is True
            assert len(receives) == 1
            assert receives[0]["line"] == 'QRESP {"ok":true,"state":"idle"}'
    finally:
        run_window.serial = original_serial


def test_qsync_preserves_asynchronous_terminal_line_and_raw_timestamps() -> None:
    terminal = b'QEVENT {"ok":true,"state":"done","suite":"core"}\n'
    reply = (
        b"QSYNC 0123456789ABCDEF 00000000000000AB "
        b"0000000000010203 0000000000010205\n"
    )
    port = FakeSerialPort([terminal, reply])
    original_serial = run_window.serial
    original_token_hex = run_window.secrets.token_hex
    try:
        run_window.serial = SimpleNamespace(Serial=lambda: port)
        run_window.secrets.token_hex = lambda _length: "0123456789abcdef"
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            timeline = run_window.BenchTimeline(root / "bench_timeline.ndjson")
            serial = run_window.BenchSerial("fake", 115200, root / "bench_serial.log", timeline)
            try:
                collector = run_window.QSyncCollector(serial)
                record = collector.collect_one("during_window")
                queued_terminal = serial.read_protocol_line(("QEVENT ",), 0.1)
            finally:
                serial.close()
                timeline.close()

            assert queued_terminal == terminal.decode().strip()
            assert port.writes == [b"QSYNC 0123456789abcdef\n"]
            assert record["status"] == "observed"
            assert record["h1_host_ns"] <= record["h4_host_ns"]
            assert record["d2_dut_us"] == 0x10203
            assert record["d3_dut_us"] == 0x10205
            assert record["clock_segment"] == str(0xAB)
            assert record["clock_segment_wire"] == "00000000000000ab"
            qsync = [
                event
                for event in read_ndjson(timeline.path)
                if event.get("event") == "qsync_exchange"
            ]
            assert len(qsync) == 1
            assert qsync[0]["h1_host_ns"] == record["h1_host_ns"]
            assert qsync[0]["h4_host_ns"] == record["h4_host_ns"]
    finally:
        run_window.serial = original_serial
        run_window.secrets.token_hex = original_token_hex


def test_qsync_recovers_from_late_nonce_reply_and_restarts_each_new_segment() -> None:
    first_nonce = "0000000000000001"
    second_nonce = "0000000000000002"
    first_reply = (
        f"QSYNC {first_nonce} 00000000000000AA "
        "0000000000001000 0000000000001001\n"
    ).encode()
    second_reply = (
        f"QSYNC {second_nonce} 00000000000000AA "
        "0000000000002000 0000000000002001\n"
    ).encode()
    port = FakeSerialPort([])
    original_serial = run_window.serial
    original_token_hex = run_window.secrets.token_hex
    nonces = iter((first_nonce, second_nonce))
    try:
        run_window.serial = SimpleNamespace(Serial=lambda: port)
        run_window.secrets.token_hex = lambda _length: next(nonces)
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            serial = run_window.BenchSerial("fake", 115200, root / "bench_serial.log")
            try:
                collector = run_window.QSyncCollector(serial)
                timed_out = collector.collect_one("first", timeout_s=0.001)
                port.responses.extend((first_reply, second_reply))
                recovered = collector.collect_one("second", timeout_s=0.1)
            finally:
                serial.close()
        assert timed_out["status"] == "failed"
        assert recovered["status"] == "observed"
        assert recovered["reply_nonce"] == second_nonce
        assert recovered["unexpected_replies"] == [
            {
                "status": "late_observed",
                "reply_nonce": first_nonce,
                "clock_segment": str(0xAA),
                "clock_segment_wire": "00000000000000aa",
                "d2_dut_us": 0x1000,
                "d3_dut_us": 0x1001,
                "h4_host_ns": recovered["unexpected_replies"][0]["h4_host_ns"],
                "h1_host_ns": timed_out["h1_host_ns"],
            }
        ]
    finally:
        run_window.serial = original_serial
        run_window.secrets.token_hex = original_token_hex

    collector = run_window.QSyncCollector(SimpleNamespace(timeline=None))
    changes = iter((True, False, True, False, False, False))
    phases: list[str] = []

    def collect(phase: str) -> dict[str, object]:
        phases.append(phase)
        return {"segment_changed": next(changes)}

    collector.collect_one = collect  # type: ignore[method-assign]
    records = collector.burst("pre_window", count=2)
    assert len(records) == 6
    assert phases == [
        "pre_window",
        "pre_window",
        "pre_window_segment_restart_1",
        "pre_window_segment_restart_1",
        "pre_window_segment_restart_2",
        "pre_window_segment_restart_2",
    ]


def test_qsync_periodic_deadline_is_anchored_within_required_cadence() -> None:
    previous_deadline = 100.0
    worst_case_exchange_completion = previous_deadline + 2.0
    next_deadline = run_window.next_qsync_deadline(
        previous_deadline,
        worst_case_exchange_completion,
    )
    assert next_deadline == previous_deadline + run_window.QSYNC_PERIOD_SECONDS

    event_loop_poll_seconds = 1.0
    earliest_spacing = next_deadline - (previous_deadline + event_loop_poll_seconds)
    latest_spacing = next_deadline + event_loop_poll_seconds - previous_deadline
    assert 5.0 <= earliest_spacing <= 10.0
    assert 5.0 <= latest_spacing <= 10.0
    assert (
        worst_case_exchange_completion
        + run_window.QSYNC_PERIOD_SECONDS
        + event_loop_poll_seconds
        - previous_deadline
        > 10.0
    ), "fixture no longer exercises the completion-anchored cadence bug"


def test_camera_first_frame_is_joinable_to_video_pts_zero() -> None:
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        timeline = run_window.BenchTimeline(root / "bench_timeline.ndjson")
        camera = CameraCapture(
            root / "camera",
            300,
            timeline_event=lambda payload: timeline.record_external(payload, "camera_recorder"),
        )
        camera.out_dir.mkdir(parents=True)
        marker = {
            "schema_version": 1,
            "event": "first_frame",
            "host_monotonic_ns": 987_654_321_000,
            "pts_zero_seconds": 0.0,
        }
        camera.first_frame_path.write_text(json.dumps(marker), encoding="utf-8")
        camera._ingest_first_frame_event()
        camera._write_result("RECORDING")
        timeline.close()

        assert camera.recording_started_monotonic == marker["host_monotonic_ns"] / 1_000_000_000
        assert json.loads(camera.result_path.read_text(encoding="utf-8"))["first_frame_event"] == marker
        observed = [event for event in read_ndjson(timeline.path) if event["event"] == "first_frame"]
        assert len(observed) == 1
        assert observed[0]["host_monotonic_ns"] == marker["host_monotonic_ns"]
        assert observed[0]["pts_zero_seconds"] == 0.0
        assert observed[0]["source"] == "camera_recorder"
        assert observed[0]["timeline_source"] == "camera_recorder"
        assert isinstance(observed[0]["observer_host_monotonic_ns"], int)


def test_build_hashes_and_imported_panic_sidecar_are_retained() -> None:
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        build = root / "build"
        output = root / "output"
        build.mkdir()
        output.mkdir()
        expected: dict[str, bytes] = {}
        for ordinal, name in enumerate(run_window.BUILD_UPLOAD_FILES, start=1):
            payload = f"artifact-{ordinal}-{name}".encode()
            expected[name] = payload
            (build / name).write_bytes(payload)

        ownership = run_window.retain_build_upload_artifacts(
            output,
            build,
            upload_performed=True,
        )
        manifest = json.loads(
            (output / run_window.BUILD_UPLOAD_ARTIFACTS_NAME).read_text(encoding="utf-8")
        )
        assert ownership["missing"] == []
        assert ownership["upload_performed"] is True
        assert manifest["upload_performed"] is True
        assert "firmware.elf" in expected
        full_elf_sha256 = hashlib.sha256(expected["firmware.elf"]).hexdigest()
        assert ownership["expected_runtime_image_id"] == full_elf_sha256[:9]
        assert manifest["expected_runtime_image_id"] == ownership["expected_runtime_image_id"]
        assert (
            manifest["expected_runtime_image_id_basis"]
            == "firmware.elf_sha256_lowercase_hex_prefix"
        )
        assert manifest["expected_runtime_image_id_hex_length"] == 9
        assert ownership["expected_runtime_image_id_basis"] == manifest[
            "expected_runtime_image_id_basis"
        ]
        assert ownership["expected_runtime_image_id_hex_length"] == manifest[
            "expected_runtime_image_id_hex_length"
        ]
        assert {
            item["name"]: item["sha256"] for item in manifest["files"]
        } == {
            name: hashlib.sha256(payload).hexdigest() for name, payload in expected.items()
        }
        assert next(
            item["sha256"] for item in manifest["files"] if item["name"] == "firmware.elf"
        ) == full_elf_sha256

        # A device reports the configured nine-character prefix, not the full
        # firmware.elf artifact hash. Keep the fixture synthetic and public.
        synthetic_full_elf_sha256 = "012345678" + ("a" * 55)
        synthetic_qstatus = {"runtimeImageId": "012345678"}
        assert len(synthetic_full_elf_sha256) == 64
        assert (
            synthetic_full_elf_sha256[:run_window.RUNTIME_IMAGE_ID_HEX_LENGTH]
            == synthetic_qstatus["runtimeImageId"]
        )

        source_csv = root / "source" / "perf_boot_42.csv"
        source_csv.parent.mkdir()
        source_csv.write_text("header\n", encoding="utf-8")
        source_sidecar = source_csv.with_suffix(".panic.jsonl")
        source_sidecar.write_text('{"event":"panic"}\n', encoding="utf-8")
        copied_csv = output / source_csv.name
        copied_csv.write_text("header\n", encoding="utf-8")
        panic = run_window.retain_import_panic_sidecar(source_csv, copied_csv)
        retained = copied_csv.with_suffix(".panic.jsonl")
        assert panic["status"] == "captured"
        assert retained.read_bytes() == source_sidecar.read_bytes()
        assert panic["sha256"] == hashlib.sha256(retained.read_bytes()).hexdigest()


def test_device_reported_causal_trace_path_is_collected_without_gating() -> None:
    calls: list[str] = []
    original_download = run_window.download_csv
    try:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)

            def download(_q: object, out_dir: Path, _timeout: int, sd_path: str) -> Path:
                calls.append(sd_path)
                path = out_dir / "causal_trace_42.csv"
                path.write_bytes(b"seq,event\n1,notification\n")
                return path

            run_window.download_csv = download
            exact_path = "/causal/causal_trace_42.csv"
            captured = run_window.collect_optional_sd_artifact(
                object(),
                root,
                30,
                exact_path,
                "causal trace",
            )
            assert calls == [exact_path]
            assert captured["status"] == "captured"
            assert captured["path"] == "causal_trace_42.csv"

            def fail(*_args: object, **_kwargs: object) -> Path:
                raise RuntimeError("fixture export failure")

            run_window.download_csv = fail
            unavailable = run_window.collect_optional_sd_artifact(
                object(),
                root,
                30,
                exact_path,
                "causal trace",
            )
            assert unavailable["status"] == "unavailable"
            assert "fixture export failure" in unavailable["reason"]
    finally:
        run_window.download_csv = original_download


def test_external_scenario_is_an_opaque_replay_argument() -> None:
    commands: list[list[str]] = []

    class FakeProcess:
        pass

    original_popen = run_window.subprocess.Popen
    try:
        run_window.subprocess.Popen = lambda command, **_kwargs: (  # type: ignore[assignment]
            commands.append(command) or FakeProcess()
        )
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            executable = root / "bin" / "v1replay"
            executable.parent.mkdir()
            executable.touch(mode=0o700)
            scenario = root / "opaque scenario.any-extension"
            out_dir = root / "replay"
            emulator = run_window.V1Emulator(
                executable,
                out_dir,
                "replay",
                scenario=str(scenario),
            )
            try:
                emulator.start()
            finally:
                if emulator.log_handle is not None:
                    emulator.log_handle.close()

            assert commands == [[
                str(executable),
                "bench",
                "--scenario",
                str(scenario),
                "--scenario-evidence",
                str(out_dir / run_window.REPLAY_SCENARIO_EVIDENCE_NAME),
                "--machine-events",
                "--owner-pid",
                str(os.getpid()),
                "--handshake-ledger",
                str(out_dir / run_window.HANDSHAKE_LEDGER_NAME),
                "--blink-profile",
                "scenario",
            ]]
    finally:
        run_window.subprocess.Popen = original_popen


def test_replay_machine_events_are_copied_once_without_reinterpretation() -> None:
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        timeline = run_window.BenchTimeline(root / "bench_timeline.ndjson")
        emulator = run_window.V1Emulator(
            root / "v1replay",
            root / "replay",
            "replay",
            machine_event=lambda payload: timeline.record_external(payload, "v1replay"),
        )
        emulator.log_path.parent.mkdir(parents=True)
        source_events = [
            {
                "state": "scenario_resolved",
                "schemaVersion": 1,
                "origin": "external",
                "sampleCount": 2,
                "sha256": "ab" * 32,
                "byteCount": 123,
            },
            {
                "state": "notification_accepted",
                "schemaVersion": 1,
                "globalTxSequence": 7,
                "characteristic": "alertData",
                "payloadSha256": "cd" * 32,
                "payloadFnv1a32": "ED74208A",
                "payloadHex": "0102",
                "hostMonotonicNs": 456_789,
            },
        ]
        emulator.log_path.write_text(
            "".join(f"V1REPLAY_EVENT {json.dumps(event)}\n" for event in source_events),
            encoding="utf-8",
        )
        emulator._observe_machine_events()
        emulator._observe_machine_events()
        timeline.close()

        copied = [
            event
            for event in read_ndjson(timeline.path)
            if event.get("source") == "v1replay"
        ]
        assert len(copied) == len(source_events)
        for original, observed in zip(source_events, copied, strict=True):
            assert all(observed[key] == value for key, value in original.items())
            assert observed["timeline_source"] == "v1replay"
            assert isinstance(observed["observer_host_monotonic_ns"], int)


def test_bench_investigation_backend_choice_is_explicit_and_non_gating() -> None:
    source = (ROOT / "bench.sh").read_text(encoding="utf-8")
    score_setup = source.index('score_args=(python3 "$ROOT_DIR/tools/bench_score.py"')
    score = source.index('-- "${score_args[@]}" || score_status=$?', score_setup)
    investigation = source.index('python3 "$ROOT_DIR/tools/bench_investigate.py"')
    final_exit = source.index('exit "$score_status"', investigation)
    invocation = source[investigation:final_exit]
    hosted_start = invocation.index('if [[ "$HOSTED_INVESTIGATOR" -eq 1 ]]; then')
    local_start = invocation.index("else\n  investigator_args+=(", hosted_start)
    choice_end = invocation.index("\nfi\n", local_start)
    hosted_branch = invocation[hosted_start:local_start]
    local_branch = invocation[local_start:choice_end]

    assert score < investigation < final_exit
    assert "HOSTED_INVESTIGATOR=0" in source
    assert "--hosted-investigator)" in source
    assert "HOSTED_INVESTIGATOR=1" in source
    assert "investigator_args+=(" in invocation
    assert "--hosted" in hosted_branch
    assert "--model gpt-5.6-sol" in hosted_branch
    assert "--local-provider" not in hosted_branch
    assert "BENCH_INVESTIGATOR_MODEL" not in hosted_branch
    assert "--hosted" not in local_branch
    assert '--local-provider "${BENCH_INVESTIGATOR_LOCAL_PROVIDER:-ollama}"' in local_branch
    assert '--model "${BENCH_INVESTIGATOR_MODEL:-qwen3-vl:8b}"' in local_branch
    assert "BENCH_INVESTIGATOR_HOSTED" not in source
    assert '"${investigator_args[@]}" || investigation_status=$?' in invocation
    assert "|| investigation_status=$?" in invocation
    assert "bench exit remains $score_status" in invocation


def main() -> int:
    tests = [
        test_serial_timeline_and_status_round_trips,
        test_serial_buffers_partial_lines_until_newline,
        test_qsync_preserves_asynchronous_terminal_line_and_raw_timestamps,
        test_qsync_recovers_from_late_nonce_reply_and_restarts_each_new_segment,
        test_qsync_periodic_deadline_is_anchored_within_required_cadence,
        test_camera_first_frame_is_joinable_to_video_pts_zero,
        test_build_hashes_and_imported_panic_sidecar_are_retained,
        test_device_reported_causal_trace_path_is_collected_without_gating,
        test_external_scenario_is_an_opaque_replay_argument,
        test_replay_machine_events_are_copied_once_without_reinterpretation,
        test_bench_investigation_backend_choice_is_explicit_and_non_gating,
    ]
    for test in tests:
        test()
    print(f"bench host evidence tests passed ({len(tests)} tests)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
