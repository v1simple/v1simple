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


def test_bench_automatic_investigation_is_local_only_and_non_gating() -> None:
    source = (ROOT / "bench.sh").read_text(encoding="utf-8")
    score_setup = source.index('score_args=(python3 "$ROOT_DIR/tools/bench_score.py"')
    score = source.index('-- "${score_args[@]}" || score_status=$?', score_setup)
    investigation = source.index('python3 "$ROOT_DIR/tools/bench_investigate.py"')
    final_exit = source.index('exit "$score_status"', investigation)
    invocation = source[investigation:final_exit]

    assert score < investigation < final_exit
    assert '--local-provider "${BENCH_INVESTIGATOR_LOCAL_PROVIDER:-ollama}"' in invocation
    assert '--model "${BENCH_INVESTIGATOR_MODEL:-qwen3-vl:8b}"' in invocation
    assert "--hosted" not in invocation
    assert "|| investigation_status=$?" in invocation
    assert "bench exit remains $score_status" in invocation


def main() -> int:
    tests = [
        test_serial_timeline_and_status_round_trips,
        test_camera_first_frame_is_joinable_to_video_pts_zero,
        test_build_hashes_and_imported_panic_sidecar_are_retained,
        test_device_reported_causal_trace_path_is_collected_without_gating,
        test_external_scenario_is_an_opaque_replay_argument,
        test_replay_machine_events_are_copied_once_without_reinterpretation,
        test_bench_automatic_investigation_is_local_only_and_non_gating,
    ]
    for test in tests:
        test()
    print(f"bench host evidence tests passed ({len(tests)} tests)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
