#!/usr/bin/env python3
"""Regression tests for the bench scorer contract."""

from __future__ import annotations

import csv
import copy
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCORER = ROOT / "tools" / "bench_score.py"
FULL_SHA = "0123456789abcdef0123456789abcdef01234567"
PRODUCT_FINGERPRINT = "a" * 64
SCENARIO_FINGERPRINT = "b" * 64
sys.path.insert(0, str(ROOT / "scripts" / "bench"))

from bench_identity import current_grader_fingerprint  # noqa: E402
from camera_artifacts import (  # noqa: E402
    build_capture_manifest,
    capture_input_hashes,
    publish_capture_manifest,
    publish_grade,
)


CURRENT_GRADER_FINGERPRINT = current_grader_fingerprint(ROOT)

ENCOUNTER_COLUMNS = (
    "millis",
    "encounter_id",
    "sample_seq",
    "event",
    "v1_index",
    "alert_count",
    "band",
    "frequency_mhz",
    "direction",
    "front_raw",
    "rear_raw",
    "front_bars",
    "rear_bars",
    "priority",
    "junk",
    "photo_type",
    "dropped_snapshots",
)

START_ALERT_REQUEST = [0xAA, 0xDA, 0xE6, 0x41, 0x01, 0xAC, 0xAB]
VERSION_REQUEST = [0xAA, 0xDA, 0xE6, 0x01, 0x01, 0x6C, 0xAB]
VERSION_RESPONSE = [
    0xAA, 0xD6, 0xEA, 0x02, 0x08,
    0x76, 0x34, 0x2E, 0x31, 0x30, 0x33, 0x38,
    0x18, 0xAB,
]
ALL_VOLUME_REQUEST = [0xAA, 0xDA, 0xE6, 0x3C, 0x01, 0xA7, 0xAB]
ALL_VOLUME_RESPONSE = [
    0xAA, 0xD6, 0xEA, 0x3D, 0x05,
    0x04, 0x00, 0x04, 0x00,
    0xB4, 0xAB,
]
FIRST_ALERT_ROW = [
    0xAA, 0xD8, 0xEA, 0x43, 0x08,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xB7, 0xAB,
]

RECONNECT_PREFLIGHT_RESULT = {
    "handshake_ready_while_alive": True,
    "serial_fence_observed": True,
    "managed_stop": True,
    "confirmed_exit": True,
    "cleanup_marker_count": 1,
    "serial_session_continuous": True,
    "boot_observed_before_second_complete": False,
}

FENCE_RESPONSE = (
    'QRESP {"ok":true,"state":"idle","suite":"core","mode":"current"}'
)
READINESS_NONCE = "0123456789abcdef0123456789abcdef"
READINESS_RESPONSE = (
    'QBSC08 {"schema":1,"nonce":"'
    + READINESS_NONCE
    + '","status":"ready"}'
)


def write_json(path: Path, payload: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")


def canonical_handshake_events(
    *,
    all_volume_before_version_response: bool = False,
    request_channel: str = "B6D4",
) -> list[dict[str, object]]:
    subscribe = {"event": "subscribe", "epoch": 1, "channel": "B2CE"}
    start = {
        "event": "request", "epoch": 1, "channel": request_channel, "bytes": START_ALERT_REQUEST,
    }
    stream = {
        "event": "stream_started", "epoch": 1, "channel": "B2CE",
        "bytes": FIRST_ALERT_ROW, "delivery": "delivered",
    }
    version_request = {
        "event": "request", "epoch": 1, "channel": request_channel, "bytes": VERSION_REQUEST,
    }
    version_response = {
        "event": "response", "epoch": 1, "channel": "B2CE",
        "bytes": VERSION_RESPONSE, "delivery": "delivered",
    }
    all_volume_request = {
        "event": "request", "epoch": 1, "channel": request_channel, "bytes": ALL_VOLUME_REQUEST,
    }
    all_volume_response = {
        "event": "response", "epoch": 1, "channel": "B2CE",
        "bytes": ALL_VOLUME_RESPONSE, "delivery": "delivered",
    }
    if all_volume_before_version_response:
        return [
            subscribe, start, stream, version_request, all_volume_request,
            all_volume_response, version_response,
        ]
    return [
        subscribe, start, stream, version_request, version_response,
        all_volume_request, all_volume_response,
    ]


def timed_handshake_events(
    start_elapsed_ms: tuple[int, ...] = (100,),
    *,
    request_channel: str = "B6D4",
) -> list[dict[str, object]]:
    assert start_elapsed_ms
    canonical = canonical_handshake_events(request_channel=request_channel)
    events = [{**canonical[0], "elapsed_ms": 0}]
    events.extend(
        {**copy.deepcopy(canonical[1]), "elapsed_ms": elapsed_ms}
        for elapsed_ms in start_elapsed_ms
    )
    elapsed_ms = start_elapsed_ms[-1] + 100
    for event in canonical[2:]:
        events.append({**copy.deepcopy(event), "elapsed_ms": elapsed_ms})
        elapsed_ms += 100
    return events


def write_handshake_ledger(
    path: Path,
    events: list[dict[str, object]] | None = None,
    *,
    schema_version: int = 1,
) -> None:
    header: dict[str, object] = {
        "schema_version": schema_version,
        "kind": "v1replay_handshake_ledger",
    }
    if schema_version == 2:
        header["timebase"] = "epoch_monotonic_ms"
    default_events = (
        timed_handshake_events()
        if schema_version == 2
        else canonical_handshake_events()
    )
    records: list[dict[str, object]] = [
        header,
        *(events if events is not None else default_events),
    ]
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        "".join(json.dumps(record, separators=(",", ":")) + "\n" for record in records),
        encoding="utf-8",
    )


def write_reconnect_logs(
    directory: Path,
    *,
    failure_kind: str = "",
) -> None:
    preflight_events = [
        {
            "state": "configured",
            "blinkProfile": "scenario",
            "blinkSource": "generated_multi_alert_assumption",
            "blinkSamples": 57,
            "totalSamples": 762,
            "cadenceHz": 3,
        }
    ]
    preflight_events.append({"state": "handshake_transport", "active": True})
    if failure_kind not in {"handshake_timeout", "handshake_invalid"}:
        preflight_events.append({"state": "handshake_ready"})
    if failure_kind == "active_session_lost":
        preflight_events.append({"state": "handshake_transport", "active": False})
    event_text = "".join(
        "V1REPLAY_EVENT " + json.dumps(event, separators=(",", ":")) + "\n"
        for event in preflight_events
    )
    if failure_kind != "handshake_timeout":
        transmissions = [
            ("B2CE", VERSION_RESPONSE),
            (
                "B4E0" if failure_kind == "handshake_invalid" else "B2CE",
                ALL_VOLUME_RESPONSE,
            ),
            ("B2CE", FIRST_ALERT_ROW),
        ]
        event_text += "".join(
            f"TX {channel} " + " ".join(f"{byte:02X}" for byte in frame) + "\n"
            for channel, frame in transmissions
        )
    (directory / "v1replay_reconnect_preflight.log").write_text(
        event_text,
        encoding="utf-8",
    )

    serial_lines: list[str] = [
        # Generic readiness traffic is deliberately outside the final nonce
        # transaction and may contain retries in a real boot sequence.
        ">>> QSTATUS",
        FENCE_RESPONSE,
        f">>> QBSC08 {READINESS_NONCE}",
        READINESS_RESPONSE,
        ">>> QSTATUS",
        FENCE_RESPONSE,
        "HOST_BOUNDARY reconnect_preflight_start",
    ]
    if failure_kind not in {"handshake_timeout", "handshake_invalid"}:
        serial_lines.extend(
            [
                "HOST_BOUNDARY reconnect_preflight_fence_begin",
                ">>> QSTATUS",
                FENCE_RESPONSE,
                "HOST_BOUNDARY reconnect_preflight_fence_complete",
            ]
        )
    if failure_kind == "cleanup_before_stop":
        serial_lines.append("[BLE] V1 disconnected; cleared LCD BLE state at 100 ms")
    serial_lines.append("HOST_BOUNDARY reconnect_preflight_process_exited")
    if failure_kind not in {"handshake_timeout", "cleanup_missing", "cleanup_before_stop"}:
        serial_lines.append("[BLE] V1 disconnected; cleared LCD BLE state at 101 ms")
    if failure_kind == "cleanup_count":
        serial_lines.extend(
            [
                "HOST_BOUNDARY reconnect_post_cleanup_fence_begin",
                ">>> QSTATUS",
                "[BLE] V1 disconnected; cleared LCD BLE state at 102 ms",
                FENCE_RESPONSE,
                "HOST_BOUNDARY reconnect_post_cleanup_fence_complete",
            ]
        )
    elif failure_kind:
        serial_lines.extend([">>> QSTATUS", FENCE_RESPONSE])
    else:
        serial_lines.extend(
            [
                "HOST_BOUNDARY reconnect_post_cleanup_fence_begin",
                ">>> QSTATUS",
                FENCE_RESPONSE,
                "HOST_BOUNDARY reconnect_post_cleanup_fence_complete",
                "HOST_BOUNDARY reconnect_pre_qstart_fence_begin",
                ">>> QSTATUS",
                FENCE_RESPONSE,
                "HOST_BOUNDARY reconnect_pre_qstart_fence_complete",
                ">>> QSTART core 300",
                'QRESP {"ok":true,"state":"running","suite":"core"}',
                'QEVENT {"ok":true,"state":"done","suite":"core","finalized":true}',
            ]
        )
    (directory / "bench_serial.log").write_text(
        "\n".join(serial_lines) + "\n",
        encoding="utf-8",
    )


def framed_bytes(destination: int, origin: int, packet_id: int, payload: list[int]) -> list[int]:
    prefix = [0xAA, destination, origin, packet_id, len(payload) + 1, *payload]
    return [*prefix, sum(prefix) & 0xFF, 0xAB]


def canonical_encounter_rows() -> list[dict[str, object]]:
    def alert(
        band: str,
        frequency_mhz: int,
        direction: str,
        front_bars: int,
        rear_bars: int,
        priority: int,
    ) -> dict[str, object]:
        return {
            "band": band,
            "frequency_mhz": frequency_mhz,
            "direction": direction,
            "front_raw": front_bars,
            "rear_raw": rear_bars,
            "front_bars": front_bars,
            "rear_bars": rear_bars,
            "priority": priority,
            "junk": 0,
            "photo_type": 0,
        }

    k_front = alert("K", 24_150, "FRONT", 1, 0, 1)
    k_side_secondary = alert("K", 24_150, "SIDE", 4, 0, 0)
    k_side_priority = alert("K", 24_150, "SIDE", 4, 0, 1)
    ka_front_five = alert("Ka", 34_700, "FRONT", 5, 0, 1)
    ka_front_six = alert("Ka", 34_700, "FRONT", 6, 0, 1)
    ka_rear_four = alert("Ka", 35_500, "REAR", 0, 4, 0)
    snapshots = (
        # A separate complete encounter proves unrelated/reconnect snapshots are allowed.
        (1, 1, "START", (k_front,)),
        (1, 2, "END", (k_front,)),
        (2, 1, "START", (k_front,)),
        (2, 2, "SAMPLE", (k_side_secondary, ka_front_five)),
        (2, 3, "SAMPLE", (k_side_secondary, ka_front_five)),
        (2, 4, "SAMPLE", (k_side_secondary, ka_front_six, ka_rear_four)),
        (2, 5, "SAMPLE", (k_side_secondary, ka_front_six, ka_rear_four)),
        (2, 6, "SAMPLE", (k_side_secondary, ka_front_five)),
        (2, 7, "SAMPLE", (k_side_secondary, ka_front_five)),
        (2, 8, "SAMPLE", (k_side_priority,)),
        (2, 9, "SAMPLE", (k_side_priority,)),
        # END repeats the previous table; it is a closure event, not a zero-row table.
        (2, 10, "END", (k_side_priority,)),
    )

    rows: list[dict[str, object]] = []
    for ordinal, (encounter_id, sample_seq, event, alerts) in enumerate(snapshots, start=1):
        for v1_index, source in enumerate(alerts, start=1):
            rows.append(
                {
                    "millis": ordinal * 17,
                    "encounter_id": encounter_id,
                    "sample_seq": sample_seq,
                    "event": event,
                    "v1_index": v1_index,
                    "alert_count": len(alerts),
                    **source,
                    "dropped_snapshots": 0,
                }
            )
    return rows


def write_encounter_csv(
    path: Path,
    rows: list[dict[str, object]] | None = None,
    columns: tuple[str, ...] = ENCOUNTER_COLUMNS,
) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as handle:
        handle.write("# encounter_schema=1,timebase=millis,v1_assignments=raw,no_gps=1,no_speed=1\n")
        writer = csv.DictWriter(handle, fieldnames=columns, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows if rows is not None else canonical_encounter_rows())


def write_replay_csv(path: Path, *, publishes: int = 708) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    exact = {
        "prioritySelectRowFlag": 708,
        "alertTablePublishes": publishes,
        "alertTablePublishes3Bogey": 30,
    }
    zero = [
        "prioritySelectFirstUsable",
        "prioritySelectFirstEntry",
        "prioritySelectAmbiguousIndex",
        "prioritySelectUnusableIndex",
        "prioritySelectInvalidChosen",
        "alertTableRowReplacements",
        "alertTableAssemblyTimeouts",
        "parserRowsBandNone",
        "parserRowsKuRaw",
        "displayLiveInvalidPrioritySkips",
        "displayLiveFallbackToUsable",
        "disc",
        "qDrop",
        "parseFail",
    ]
    columns = ["millis", *exact, *zero, "v1AllVolumeParsed"]
    qstart = {column: 0 for column in columns}
    qstart["millis"] = 10
    replacement = {**qstart, "millis": 20}
    end = {**qstart, "millis": 300_000, **exact, "v1AllVolumeParsed": 1}
    path.write_text(
        ",".join(columns)
        + "\n#session_start,seq=1,bootId=1,uptime_ms=10,token=QSTART,schema=46\n"
        + ",".join(str(qstart[column]) for column in columns)
        + "\n"
        + ",".join(columns)
        + "\n#session_start,seq=2,bootId=1,uptime_ms=20,token=REPLACEMENT,schema=46\n"
        + ",".join(str(replacement[column]) for column in columns)
        + "\n"
        + ",".join(str(end[column]) for column in columns)
        + "\n",
        encoding="utf-8",
    )


def rewrite_replay_all_volume(
    path: Path,
    values: list[str | int],
    *,
    counter_name: str = "v1AllVolumeParsed",
) -> None:
    lines = path.read_text(encoding="utf-8").splitlines()
    data_indices = [
        index
        for index, line in enumerate(lines)
        if line and not line.startswith("#") and not line.startswith("millis,")
    ]
    assert_true(len(data_indices) == len(values), f"unexpected replay fixture shape: {lines}")
    for index, value in zip(data_indices, values):
        fields = lines[index].split(",")
        fields[-1] = str(value)
        lines[index] = ",".join(fields)
    if counter_name != "v1AllVolumeParsed":
        lines = [
            line.replace("v1AllVolumeParsed", counter_name)
            if line.startswith("millis,")
            else line
            for line in lines
        ]
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def write_window(
    root: Path,
    suite: str,
    *,
    hard: int = 0,
    advisory: int = 0,
    result: str = "NO_BASELINE",
    replay_publishes: int = 708,
    replay_completed: bool = True,
    camera_result: str = "",
    camera_grade_result: str = "PASS",
    camera_grade_errors: tuple[str, ...] = (),
) -> None:
    step = root / suite
    encounter_path: Path | None = None
    metric_payload = [
        {
            "metric": "ble_process_max_peak_us",
            "score_status": "pass",
            "absolute_state": "pass",
            "score_level": "hard",
            "current_value": 60000,
            "unit": "us",
            "messages": [],
        },
        {
            "metric": "display_preview_render_peak_us",
            "score_status": "info",
            "absolute_state": "missing",
            "score_level": "advisory",
            "required": False,
            "current_value": None,
            "unit": "us",
            "messages": ["metric missing from run output for applicable track"],
        },
    ]
    if hard:
        metric_payload.append(
            {
                "metric": "queue_drops_delta",
                "score_status": "fail",
                "absolute_state": "fail",
                "score_level": "hard",
                "current_value": 1,
                "messages": ["value 1 above max 0"],
            }
        )
    window_payload = {
        "schema_version": 1,
        "result": "COLLECTED",
        "suite": suite,
        "product_fingerprint": PRODUCT_FINGERPRINT,
        "scenario_fingerprint": SCENARIO_FINGERPRINT,
        "git_worktree_clean": True,
        "scoring_path": str(step / "scoring.json"),
        "manifest_path": str(step / "manifest.json"),
    }
    if suite == "replay":
        csv_path = step / "perf.csv"
        encounter_path = step / "encounters_test.csv"
        handshake_path = step / "handshake_ledger.jsonl"
        reconnect_preflight_handshake_path = step / "handshake_ledger_preflight.jsonl"
        write_replay_csv(csv_path, publishes=replay_publishes)
        write_encounter_csv(encounter_path)
        write_handshake_ledger(handshake_path)
        write_handshake_ledger(reconnect_preflight_handshake_path)
        write_reconnect_logs(step)
        window_payload.update(
            {
                "csv_path": str(csv_path),
                "encounter_csv_path": str(encounter_path),
                "handshake_ledger_path": str(handshake_path),
                "reconnect_preflight_handshake_ledger_path": str(
                    reconnect_preflight_handshake_path
                ),
                "reconnect_preflight": dict(RECONNECT_PREFLIGHT_RESULT),
                "reconnect_preflight_log_path": str(
                    step / "v1replay_reconnect_preflight.log"
                ),
                "bench_serial_log_path": str(step / "bench_serial.log"),
                "duration_seconds": 300,
                "segment": "last",
                "replay": {
                    "started": True,
                    "completed": replay_completed,
                    "returncode": 0 if replay_completed else 2,
                },
            }
        )
    write_json(
        step / "window_result.json",
        window_payload,
    )
    write_json(
        step / "manifest.json",
        {
            "schema_version": 1,
            "git_sha": FULL_SHA,
            "git_ref": "dev/test",
            "product_fingerprint": PRODUCT_FINGERPRINT,
            "scenario_fingerprint": SCENARIO_FINGERPRINT,
            "rows": 61,
            "duration_s": 300.0,
        },
    )
    write_json(
        step / "scoring.json",
        {
            "schema_version": 1,
            "result": result,
            "summary": {
                "metrics_scored": 10,
                "hard_failures": hard,
                "advisory_failures": advisory,
            },
            "metrics": metric_payload,
        },
    )
    if camera_result:
        camera_dir = step / "camera"
        camera_dir.mkdir(parents=True, exist_ok=True)
        (camera_dir / "capture_manifest.json").unlink(missing_ok=True)
        (camera_dir / "grades" / f"{CURRENT_GRADER_FINGERPRINT}.json").unlink(missing_ok=True)
        evidence_names = {
            "video": "evidence_exp50.mov",
            "session_start_still": "session_start_exp50.jpg",
            "bright_still": "final_auto.jpg",
            "dim_still": "final_profile.jpg",
        }
        if camera_result == "CAPTURED":
            for name in evidence_names.values():
                (camera_dir / name).write_bytes(b"evidence")
        physical_camera = {
            "schema_version": 1,
            "kind": "bench_camera_evidence",
            "result": camera_result,
            "camera_name": "Global Shutter Camera",
            "camera_device_index": 0,
            "profile": {"focus_abs": 306, "video_exposure_time_abs": 50},
            "expected_duration_seconds": 300,
            "profile_validation": {"result": "PASS"},
            **{key: value if camera_result == "CAPTURED" else "" for key, value in evidence_names.items()},
            "video_duration_seconds": 300.0 if camera_result == "CAPTURED" else 0.0,
            "errors": [] if camera_result == "CAPTURED" else ["camera unavailable"],
        }
        write_json(camera_dir / "camera_result.json", physical_camera)
        if camera_result == "CAPTURED" and camera_grade_result:
            capture = build_capture_manifest(
                camera_dir=camera_dir,
                camera_result=physical_camera,
                suite=suite,
                product_fingerprint=PRODUCT_FINGERPRINT,
                scenario_fingerprint=SCENARIO_FINGERPRINT,
                encounter_csv_path=encounter_path,
                timing_anchor={"kind": "first_emitted_replay_sample", "video_seconds": 8.0}
                if suite == "replay"
                else None,
            )
            publish_capture_manifest(camera_dir, capture)
            diagnostics = (
                [{"code": "fixture_inconclusive", "message": message} for message in camera_grade_errors]
                if camera_grade_result == "INCONCLUSIVE"
                else []
            )
            grade = {
                "schema_version": 4,
                "kind": "bench_camera_grade",
                "capture_id": capture["capture_id"],
                "grader_fingerprint": CURRENT_GRADER_FINGERPRINT,
                "grade_id": "c" * 64,
                "input_hashes": capture_input_hashes(capture),
                "suite": suite,
                "video": evidence_names["video"],
                "result": camera_grade_result,
                "confidence": {
                    "result": "INCONCLUSIVE" if camera_grade_result == "INCONCLUSIVE" else "PASS",
                    "gates": {},
                },
                "checks": {
                    "display_matches_log": {
                        "result": camera_grade_result,
                        "ratio": 1.0 if camera_grade_result == "PASS" else 0.0,
                    }
                },
                "diagnostics": diagnostics,
                "errors": list(camera_grade_errors) if camera_grade_result == "FAIL" else [],
            }
            grade_path, _created = publish_grade(
                camera_dir,
                capture,
                CURRENT_GRADER_FINGERPRINT,
                grade,
            )
            window_payload["camera"] = {
                "capture_manifest": "capture_manifest.json",
                "capture_id": capture["capture_id"],
                "grade": grade_path.relative_to(camera_dir).as_posix(),
            }
            write_json(step / "window_result.json", window_payload)


def run_score(
    root: Path,
    *suites: str,
    camera_suites: tuple[str, ...] = (),
    out_path: Path | None = None,
) -> subprocess.CompletedProcess[str]:
    cmd = [sys.executable, str(SCORER), "--run-dir", str(root)]
    for suite in suites:
        cmd.extend(["--suite", suite])
    for suite in camera_suites:
        cmd.extend(["--camera-suite", suite])
    if out_path is not None:
        cmd.extend(["--out", str(out_path)])
    return subprocess.run(cmd, cwd=ROOT, text=True, capture_output=True, check=False)


def assert_true(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def test_no_baseline_language_does_not_make_bench_fail() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        write_window(root, "core", result="NO_BASELINE")
        write_window(root, "display", result="NO_BASELINE")
        proc = run_score(root, "core", "display")
        assert_true(proc.returncode == 0, proc.stdout + proc.stderr)
        result = json.loads((root / "bench_result.json").read_text(encoding="utf-8"))
        assert_true(result["result"] == "PASS", f"unexpected result: {result}")
        assert_true(result["schema_version"] == 3, f"unexpected schema: {result}")
        assert_true(result["git_sha"] == FULL_SHA, f"missing full Git binding: {result}")
        assert_true(result["git_worktree_clean"] is True, f"dirty binding: {result}")
        assert_true("NO_BASELINE" not in proc.stdout, f"bench output leaked old baseline language: {proc.stdout}")
        assert_true("top budget pressure:" in proc.stdout, f"bench output should surface budget pressure: {proc.stdout}")
        assert_true("ble_process_max_peak_us" in proc.stdout, f"bench output should name top pressure metric: {proc.stdout}")
        assert_true("display_preview_render_peak_us" not in proc.stdout, f"optional missing metrics are not actionable PASS evidence: {proc.stdout}")


def test_baseline_only_regression_is_comparison_not_verdict() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        write_window(root, "core")
        scoring_path = root / "core" / "scoring.json"
        scoring = json.loads(scoring_path.read_text(encoding="utf-8"))
        scoring["summary"]["hard_failures"] = 1
        scoring["metrics"].append(
            {
                "metric": "parse_successes_delta",
                "score_level": "hard",
                "score_status": "fail",
                "absolute_state": "pass",
                "regression_state": "fail",
                "current_value": 900,
                "messages": ["value regressed below local baseline"],
            }
        )
        write_json(scoring_path, scoring)
        proc = run_score(root, "core")
        assert_true(proc.returncode == 0, proc.stdout + proc.stderr)
        assert_true("local baseline" not in proc.stdout, proc.stdout)


def test_required_missing_metric_remains_a_hard_failure() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        write_window(root, "core")
        scoring_path = root / "core" / "scoring.json"
        scoring = json.loads(scoring_path.read_text(encoding="utf-8"))
        scoring["metrics"].append(
            {
                "metric": "queue_drops_delta",
                "score_level": "hard",
                "required": True,
                "score_status": "fail",
                "absolute_state": "missing",
                "current_value": None,
                "messages": ["metric missing from run output"],
            }
        )
        write_json(scoring_path, scoring)
        proc = run_score(root, "core")
        assert_true(proc.returncode == 2, proc.stdout + proc.stderr)
        assert_true("queue_drops_delta" in proc.stdout, proc.stdout)


def test_failed_base_result_remains_a_hard_failure() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        write_window(root, "core")
        scoring_path = root / "core" / "scoring.json"
        scoring = json.loads(scoring_path.read_text(encoding="utf-8"))
        scoring["manifest"] = {"base_result": "FAIL"}
        write_json(scoring_path, scoring)
        proc = run_score(root, "core")
        assert_true(proc.returncode == 2, proc.stdout + proc.stderr)
        assert_true("failed base result" in proc.stdout, proc.stdout)


def test_core_metric_failure_is_actionable_failure() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        write_window(root, "core", hard=1, result="FAIL")
        write_window(root, "display")
        proc = run_score(root, "core", "display")
        assert_true(proc.returncode == 2, proc.stdout + proc.stderr)
        assert_true("\nfailed:\n" in proc.stdout, proc.stdout)
        assert_true("core.queue_drops_delta" in proc.stdout, proc.stdout)


def test_display_metric_failure_is_actionable_failure() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        write_window(root, "core")
        write_window(root, "display", hard=1, result="FAIL")
        proc = run_score(root, "core", "display")
        assert_true(proc.returncode == 2, proc.stdout + proc.stderr)
        assert_true("display.queue_drops_delta" in proc.stdout, proc.stdout)


def test_advisory_absolute_bound_is_a_warning() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        write_window(root, "core")
        scoring_path = root / "core" / "scoring.json"
        scoring = json.loads(scoring_path.read_text(encoding="utf-8"))
        scoring["metrics"].append(
            {
                "metric": "sd_runtime_max_peak_us",
                "score_level": "hard",
                "required": True,
                "score_status": "warn",
                "absolute_state": "pass",
                "advisory_state": "fail",
                "current_value": 75000,
                "messages": ["value 75000 above advisory max 60000"],
            }
        )
        write_json(scoring_path, scoring)
        proc = run_score(root, "core")
        assert_true(proc.returncode == 1, proc.stdout + proc.stderr)
        result = json.loads((root / "bench_result.json").read_text(encoding="utf-8"))
        assert_true(result["result"] == "WARN", f"unexpected advisory result: {result}")
        assert_true("\nwarnings:\n" in proc.stdout, proc.stdout)
        assert_true("\nfailed:\n" not in proc.stdout, proc.stdout)
        assert_true("sd_runtime_max_peak_us" in proc.stdout, proc.stdout)


def test_custom_output_preserves_canonical_summary_pair() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        write_window(root, "core")
        custom_result_path = root / "bench_result_regraded.json"
        custom = run_score(root, "core", out_path=custom_result_path)
        assert_true(custom.returncode == 0, custom.stdout + custom.stderr)
        assert_true(
            not (root / "bench_summary.txt").exists(),
            "custom output created a canonical bench summary without a canonical result",
        )

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        write_window(root, "core")
        initial = run_score(root, "core")
        assert_true(initial.returncode == 0, initial.stdout + initial.stderr)
        canonical_result_path = root / "bench_result.json"
        canonical_summary_path = root / "bench_summary.txt"
        canonical_result = canonical_result_path.read_text(encoding="utf-8")
        canonical_summary = canonical_summary_path.read_text(encoding="utf-8")

        collision = run_score(root, "core", out_path=canonical_summary_path)
        assert_true(collision.returncode == 2, collision.stdout + collision.stderr)
        assert_true(
            "must not resolve to the canonical bench_summary.txt" in collision.stderr,
            collision.stderr,
        )
        assert_true(
            canonical_result_path.read_text(encoding="utf-8") == canonical_result,
            "summary collision rewrote the canonical bench result",
        )
        assert_true(
            canonical_summary_path.read_text(encoding="utf-8") == canonical_summary,
            "summary collision rewrote the canonical bench summary",
        )

        write_window(root, "core", hard=1, result="FAIL")
        custom_result_path = root / "bench_result_regraded.json"
        regraded = run_score(root, "core", out_path=custom_result_path)
        assert_true(regraded.returncode == 2, regraded.stdout + regraded.stderr)
        custom_result = json.loads(custom_result_path.read_text(encoding="utf-8"))
        assert_true(custom_result["result"] == "FAIL", f"unexpected custom result: {custom_result}")
        assert_true(
            canonical_result_path.read_text(encoding="utf-8") == canonical_result,
            "custom output rewrote the canonical bench result",
        )
        assert_true(
            canonical_summary_path.read_text(encoding="utf-8") == canonical_summary,
            "custom output rewrote the canonical bench summary",
        )

        canonical_result_alias = root / "canonical_result_alias.json"
        canonical_result_alias.symlink_to(canonical_result_path)
        explicit_canonical = run_score(root, "core", out_path=canonical_result_alias)
        assert_true(
            explicit_canonical.returncode == 2,
            explicit_canonical.stdout + explicit_canonical.stderr,
        )
        updated_result = json.loads(canonical_result_path.read_text(encoding="utf-8"))
        updated_summary = canonical_summary_path.read_text(encoding="utf-8")
        assert_true(updated_result["result"] == "FAIL", f"unexpected result: {updated_result}")
        assert_true("bench result: FAIL" in updated_summary, updated_summary)


def test_missing_window_artifact_is_collection_failure() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        write_window(root, "display")
        proc = run_score(root, "core", "display")
        assert_true(proc.returncode == 3, proc.stdout + proc.stderr)
        assert_true("collection: FAIL" in proc.stdout, proc.stdout)
        assert_true("missing or invalid" in proc.stdout, proc.stdout)


def test_replay_exact_invariants_are_part_of_the_verdict() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        write_window(root, "replay")
        proc = run_score(root, "replay")
        assert_true(proc.returncode == 0, proc.stdout + proc.stderr)
        result = json.loads((root / "bench_result.json").read_text(encoding="utf-8"))
        checks = result["windows"][0]["replay_checks"]
        assert_true(checks["result"] == "PASS", f"unexpected replay checks: {checks}")
        assert_true(checks["observed_deltas"]["alertTablePublishes"] == 708, f"wrong replay deltas: {checks}")
        all_volume = checks["all_volume_consumption"]
        assert_true(all_volume["result"] == "PASS", f"missing parse proof: {all_volume}")
        assert_true(all_volume["observed_delta"] == 1, f"wrong parse delta: {all_volume}")
        assert_true(all_volume["selected_schema"] == 46, f"wrong parse schema: {all_volume}")
        encounter = checks["encounter_checks"]
        assert_true(encounter["result"] == "PASS", f"unexpected encounter checks: {encounter}")
        assert_true(encounter["matched_checkpoints"] == 4, f"missing checkpoints: {encounter}")
        assert_true(encounter["closure_found"] is True, f"missing END closure: {encounter}")
        handshake = checks["handshake_checks"]
        assert_true(handshake["result"] == "PASS", f"unexpected handshake checks: {handshake}")
        assert_true(handshake["complete_epoch"] == 1, f"missing complete epoch: {handshake}")
        reconnect = checks["reconnect_checks"]
        assert_true(reconnect["result"] == "PASS", f"unexpected reconnect checks: {reconnect}")
        assert_true(
            reconnect["preflight_handshake_checks"]["event_count"] == 7,
            f"unexpected reconnect preflight: {reconnect}",
        )
        assert_true(
            reconnect["primary_handshake_checks"]["event_count"] == 7,
            f"unexpected reconnect primary: {reconnect}",
        )
        assert_true(
            reconnect["lifecycle_checks"]["result"] == "PASS",
            f"unexpected reconnect lifecycle: {reconnect}",
        )
        assert_true(
            reconnect["lifecycle_checks"]["diagnostics"] == [],
            f"healthy reconnect gained lifecycle diagnostics: {reconnect}",
        )


def test_replay_all_volume_consumption_mutants_are_fail_closed() -> None:
    def score_with_values(values: list[str | int]) -> tuple[subprocess.CompletedProcess[str], dict]:
        root = Path(tmp)
        write_window(root, "replay")
        rewrite_replay_all_volume(root / "replay" / "perf.csv", values)
        proc = run_score(root, "replay")
        result = json.loads((root / "bench_result.json").read_text(encoding="utf-8"))
        return proc, result["windows"][0]["replay_checks"]

    for values, expected_delta in (([0, 0, 0], 0), ([0, 0, 2], 2)):
        with tempfile.TemporaryDirectory() as tmp:
            proc, checks = score_with_values(values)
            assert_true(proc.returncode == 2, proc.stdout + proc.stderr)
            all_volume = checks["all_volume_consumption"]
            assert_true(all_volume["result"] == "FAIL", f"unexpected parse gate: {checks}")
            assert_true(all_volume["observed_delta"] == expected_delta, f"wrong delta: {checks}")
            assert_true(
                f"v1AllVolumeParsed delta={expected_delta} expected=1" in proc.stdout,
                proc.stdout,
            )

    # The counter is boot-cumulative: a parse in preflight may establish a
    # nonzero QSTART baseline, while B must still add exactly one fresh parse.
    with tempfile.TemporaryDirectory() as tmp:
        proc, checks = score_with_values([1, 1, 2])
        assert_true(proc.returncode == 0, proc.stdout + proc.stderr)
        all_volume = checks["all_volume_consumption"]
        assert_true(all_volume["result"] == "PASS", f"inherited baseline failed: {checks}")
        assert_true(all_volume["qstart_value"] == 1, f"baseline was not retained: {checks}")
        assert_true(all_volume["replacement_value"] == 2, f"fresh B parse missing: {checks}")
        assert_true(all_volume["observed_delta"] == 1, f"wrong fresh delta: {checks}")

    # A parse earned before QSTART is only the baseline. It cannot satisfy the
    # replacement connection's independently scoped consumption requirement.
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        write_window(root, "replay")
        csv_path = root / "replay" / "perf.csv"
        rewrite_replay_all_volume(csv_path, [1, 1, 1])
        lines = csv_path.read_text(encoding="utf-8").splitlines()
        lines[1] = lines[1].replace("seq=1", "seq=2")
        lines[4] = lines[4].replace("seq=2", "seq=3")
        zero_fields = lines[2].split(",")
        zero_fields[0] = "1"
        zero_fields[-1] = "0"
        earned_fields = list(zero_fields)
        earned_fields[0] = "5"
        earned_fields[-1] = "1"
        preflight = [
            lines[0],
            "#session_start,seq=1,bootId=1,uptime_ms=1,token=PREFLIGHT,schema=46",
            ",".join(zero_fields),
            ",".join(earned_fields),
        ]
        csv_path.write_text("\n".join([*preflight, *lines]) + "\n", encoding="utf-8")
        proc = run_score(root, "replay")
        assert_true(proc.returncode == 2, proc.stdout + proc.stderr)
        result = json.loads((root / "bench_result.json").read_text(encoding="utf-8"))
        checks = result["windows"][0]["replay_checks"]
        assert_true(checks["session_indices"] == [2, 3], f"preflight leaked into scope: {checks}")
        assert_true(checks["all_volume_consumption"]["observed_delta"] == 0, f"false credit: {checks}")

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        write_window(root, "replay")
        rewrite_replay_all_volume(
            root / "replay" / "perf.csv",
            [0, 0, 1],
            counter_name="v1AllVolumeParseCount",
        )
        proc = run_score(root, "replay")
        assert_true(proc.returncode == 3, proc.stdout + proc.stderr)
        assert_true("missing required columns: v1AllVolumeParsed" in proc.stdout, proc.stdout)

    for session_name, header_index, row_indices in (
        ("QSTART", 0, (2,)),
        ("replacement", 3, (5, 6)),
    ):
        for conflicting in (False, True):
            with tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                write_window(root, "replay")
                csv_path = root / "replay" / "perf.csv"
                lines = csv_path.read_text(encoding="utf-8").splitlines()
                lines[header_index] += ",v1AllVolumeParsed"
                for row_index in row_indices:
                    original = int(lines[row_index].rsplit(",", 1)[1])
                    duplicate = original + 1 if conflicting else original
                    lines[row_index] += f",{duplicate}"
                csv_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
                proc = run_score(root, "replay")
                assert_true(proc.returncode == 3, proc.stdout + proc.stderr)
                assert_true(
                    "selected session header repeats required column: v1AllVolumeParsed"
                    in proc.stdout,
                    f"{session_name} duplicate false-greened: {proc.stdout}",
                )

    with tempfile.TemporaryDirectory() as tmp:
        proc, _checks = score_with_values([0, 0, "not-a-number"])
        assert_true(proc.returncode == 3, proc.stdout + proc.stderr)
        assert_true("replay CSV could not be read" in proc.stdout, proc.stdout)

    with tempfile.TemporaryDirectory() as tmp:
        proc, checks = score_with_values([0, 0, "1.5"])
        assert_true(proc.returncode == 3, proc.stdout + proc.stderr)
        assert_true("counter is not an unsigned integer" in proc.stdout, proc.stdout)
        assert_true(checks["all_volume_consumption"]["result"] == "COLLECTION_FAILED", f"wrong class: {checks}")

    with tempfile.TemporaryDirectory() as tmp:
        proc, checks = score_with_values([0x1_0000_0000, 0x1_0000_0000, 0x1_0000_0001])
        assert_true(proc.returncode == 3, proc.stdout + proc.stderr)
        assert_true("counter is not an unsigned integer" in proc.stdout, proc.stdout)
        assert_true(checks["all_volume_consumption"]["result"] == "COLLECTION_FAILED", f"wrong class: {checks}")

    with tempfile.TemporaryDirectory() as tmp:
        proc, checks = score_with_values([2, 2, 1])
        assert_true(proc.returncode == 3, proc.stdout + proc.stderr)
        assert_true("v1AllVolumeParsed" in proc.stdout and "cumulative counters regress" in proc.stdout, proc.stdout)
        assert_true(checks["all_volume_consumption"]["result"] == "COLLECTION_FAILED", f"wrong class: {checks}")

    for schemas in ((45, 45), (46, 47)):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            write_window(root, "replay")
            csv_path = root / "replay" / "perf.csv"
            lines = csv_path.read_text(encoding="utf-8").splitlines()
            lines[1] = lines[1].replace("schema=46", f"schema={schemas[0]}")
            lines[4] = lines[4].replace("schema=46", f"schema={schemas[1]}")
            csv_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
            proc = run_score(root, "replay")
            assert_true(proc.returncode == 3, proc.stdout + proc.stderr)
            expected = "requires session schema >=46" if schemas == (45, 45) else "invalid or discontinuous metadata"
            assert_true(expected in proc.stdout, proc.stdout)

    # Transport delivery and firmware consumption remain an AND-join: neither
    # artifact can substitute for the other.
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        write_window(root, "replay")
        events = copy.deepcopy(canonical_handshake_events())
        events[-1]["bytes"][5] = 0x05
        write_handshake_ledger(root / "replay" / "handshake_ledger.jsonl", events)
        proc = run_score(root, "replay")
        assert_true(proc.returncode == 2, proc.stdout + proc.stderr)
        result = json.loads((root / "bench_result.json").read_text(encoding="utf-8"))
        checks = result["windows"][0]["replay_checks"]
        assert_true(checks["all_volume_consumption"]["result"] == "PASS", f"CSV proof changed: {checks}")
        assert_true(checks["handshake_checks"]["result"] == "FAIL", f"raw mutant passed: {checks}")

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        write_window(root, "replay")
        (root / "replay" / "handshake_ledger.jsonl").unlink()
        proc = run_score(root, "replay")
        assert_true(proc.returncode == 3, proc.stdout + proc.stderr)
        result = json.loads((root / "bench_result.json").read_text(encoding="utf-8"))
        checks = result["windows"][0]["replay_checks"]
        assert_true(checks["all_volume_consumption"]["result"] == "PASS", f"CSV proof changed: {checks}")
        assert_true(checks["handshake_checks"]["result"] == "COLLECTION_FAILED", f"missing raw passed: {checks}")

    with tempfile.TemporaryDirectory() as tmp:
        proc, checks = score_with_values([0, 0, 0])
        assert_true(proc.returncode == 2, proc.stdout + proc.stderr)
        assert_true(checks["handshake_checks"]["result"] == "PASS", f"raw proof changed: {checks}")
        assert_true(checks["all_volume_consumption"]["result"] == "FAIL", f"missing parse passed: {checks}")


def test_replay_handshake_accepts_independent_reply_order_and_complete_reconnect_epoch() -> None:
    for request_channel in ("B6D4", "BAD4"):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            write_window(root, "replay")
            write_handshake_ledger(
                root / "replay" / "handshake_ledger.jsonl",
                canonical_handshake_events(
                    all_volume_before_version_response=True,
                    request_channel=request_channel,
                ),
            )
            proc = run_score(root, "replay")
            assert_true(
                proc.returncode == 0,
                f"{request_channel}: {proc.stdout}{proc.stderr}",
            )

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        write_window(root, "replay")
        events = canonical_handshake_events()
        stream = events.pop(2)
        events.append(stream)
        write_handshake_ledger(root / "replay" / "handshake_ledger.jsonl", events)
        proc = run_score(root, "replay")
        assert_true(proc.returncode == 0, proc.stdout + proc.stderr)

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        write_window(root, "replay")
        incomplete = canonical_handshake_events()[:2]
        complete = copy.deepcopy(canonical_handshake_events())
        for event in complete:
            event["epoch"] = 2
        write_handshake_ledger(root / "replay" / "handshake_ledger.jsonl", incomplete + complete)
        proc = run_score(root, "replay")
        assert_true(proc.returncode == 2, proc.stdout + proc.stderr)
        result = json.loads((root / "bench_result.json").read_text(encoding="utf-8"))
        handshake = result["windows"][0]["replay_checks"]["handshake_checks"]
        reconnect = result["windows"][0]["replay_checks"]["reconnect_checks"]
        assert_true(handshake["result"] == "PASS", f"base handshake behavior changed: {handshake}")
        assert_true(handshake["complete_epoch"] == 2, f"reconnect epoch was not accepted: {handshake}")
        assert_true(reconnect["result"] == "FAIL", f"strict reconnect shape passed: {reconnect}")
        assert_true("epoch_count=2 expected=1" in proc.stdout, proc.stdout)


def test_timed_handshake_start_retry_contract_is_fail_closed() -> None:
    def exercise(
        events: list[dict[str, object]],
        *,
        schema_version: int = 2,
        timebase: str | None = "epoch_monotonic_ms",
    ) -> tuple[subprocess.CompletedProcess[str], dict, dict]:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            write_window(root, "replay")
            write_handshake_ledger(
                root / "replay" / "handshake_ledger.jsonl",
                events,
                schema_version=schema_version,
            )
            ledger_path = root / "replay" / "handshake_ledger.jsonl"
            if schema_version == 2 and timebase != "epoch_monotonic_ms":
                records = [
                    json.loads(line)
                    for line in ledger_path.read_text(encoding="utf-8").splitlines()
                ]
                if timebase is None:
                    records[0].pop("timebase")
                else:
                    records[0]["timebase"] = timebase
                ledger_path.write_text(
                    "".join(
                        json.dumps(record, separators=(",", ":")) + "\n"
                        for record in records
                    ),
                    encoding="utf-8",
                )
            proc = run_score(root, "replay")
            result = json.loads((root / "bench_result.json").read_text(encoding="utf-8"))
            replay = result["windows"][0]["replay_checks"]
            reconnect = replay["reconnect_checks"]["primary_handshake_checks"]
            return proc, replay["handshake_checks"], reconnect

    proc, handshake, reconnect = exercise(canonical_handshake_events(), schema_version=1)
    assert_true(proc.returncode == 0, proc.stdout + proc.stderr)
    assert_true(
        handshake["schema_version"] == 1
        and handshake["handshake_state"] == "complete"
        and reconnect["event_count"] == 7,
        f"legacy canonical handshake changed: {handshake} {reconnect}",
    )

    legacy_retry = canonical_handshake_events()
    legacy_retry.insert(2, copy.deepcopy(legacy_retry[1]))
    proc, handshake, _ = exercise(legacy_retry, schema_version=1)
    assert_true(proc.returncode == 2, proc.stdout + proc.stderr)
    assert_true(handshake["handshake_state"] == "invalid", f"legacy retry passed: {handshake}")
    assert_true("repeats start request" in proc.stdout, proc.stdout)

    for retry_gap_ms, expected_exit in ((999, 2), (1000, 0), (1001, 0)):
        events = timed_handshake_events((100, 100 + retry_gap_ms))
        proc, handshake, reconnect = exercise(events)
        assert_true(
            proc.returncode == expected_exit,
            f"gap={retry_gap_ms}: {proc.stdout}{proc.stderr}",
        )
        expected_state = "invalid" if expected_exit else "complete"
        assert_true(
            handshake["handshake_state"] == expected_state,
            f"gap={retry_gap_ms}: {handshake}",
        )
        if expected_exit == 0:
            assert_true(
                reconnect["result"] == "PASS" and reconnect["event_count"] == 8,
                f"timed retry failed strict reconnect scoring: {reconnect}",
            )
        else:
            assert_true("less than 1000 ms apart" in proc.stdout, proc.stdout)

    proc, handshake, _ = exercise(timed_handshake_events((100, 1100, 2099)))
    assert_true(proc.returncode == 2, proc.stdout + proc.stderr)
    assert_true(handshake["handshake_state"] == "invalid", f"consecutive-gap mutant passed: {handshake}")
    assert_true("less than 1000 ms apart" in proc.stdout, proc.stdout)

    post_stream = timed_handshake_events()
    retry = copy.deepcopy(post_stream[1])
    retry["elapsed_ms"] = int(post_stream[-1]["elapsed_ms"]) + 1000
    post_stream.append(retry)
    proc, handshake, _ = exercise(post_stream)
    assert_true(proc.returncode == 2, proc.stdout + proc.stderr)
    assert_true(handshake["handshake_state"] == "invalid", f"post-stream retry passed: {handshake}")
    assert_true("after stream delivery" in proc.stdout, proc.stdout)

    five_starts = tuple(100 + 1000 * index for index in range(5))
    proc, handshake, reconnect = exercise(timed_handshake_events(five_starts))
    assert_true(proc.returncode == 0, proc.stdout + proc.stderr)
    assert_true(
        handshake["handshake_state"] == "complete"
        and reconnect["result"] == "PASS"
        and reconnect["event_count"] == 11,
        f"bounded retries failed: {handshake} {reconnect}",
    )

    six_starts = tuple(100 + 1000 * index for index in range(6))
    proc, handshake, reconnect = exercise(timed_handshake_events(six_starts))
    assert_true(proc.returncode == 2, proc.stdout + proc.stderr)
    assert_true(
        handshake["handshake_state"] == "invalid" and reconnect["event_count"] == 12,
        f"unbounded retry mutant passed: {handshake} {reconnect}",
    )
    assert_true("exceeds its bounded start request count" in proc.stdout, proc.stdout)

    switched_channel = timed_handshake_events((100, 1100))
    switched_channel[2]["channel"] = "BAD4"
    proc, handshake, _ = exercise(switched_channel)
    assert_true(proc.returncode == 2, proc.stdout + proc.stderr)
    assert_true(handshake["handshake_state"] == "invalid", f"channel switch passed: {handshake}")
    assert_true("switches its selected command channel" in proc.stdout, proc.stdout)

    incomplete = timed_handshake_events((100, 1100))
    incomplete.pop(3)
    proc, handshake, _ = exercise(incomplete)
    assert_true(proc.returncode == 2, proc.stdout + proc.stderr)
    assert_true(
        handshake["handshake_state"] == "incomplete",
        f"missing delivery was not incomplete: {handshake}",
    )

    malformed_elapsed: tuple[object, ...] = (None, True, 1.5, -1, 0x100000000)
    for value in malformed_elapsed:
        events = timed_handshake_events()
        if value is None:
            events[1].pop("elapsed_ms")
        else:
            events[1]["elapsed_ms"] = value
        proc, handshake, _ = exercise(events)
        assert_true(proc.returncode == 3, f"elapsed_ms={value!r}: {proc.stdout}{proc.stderr}")
        assert_true(
            handshake["result"] == "COLLECTION_FAILED",
            f"elapsed_ms={value!r} did not fail closed: {handshake}",
        )

    for timebase in (None, "wall_clock_ms"):
        proc, handshake, _ = exercise(timed_handshake_events(), timebase=timebase)
        assert_true(proc.returncode == 3, f"timebase={timebase!r}: {proc.stdout}{proc.stderr}")
        assert_true(
            handshake["result"] == "COLLECTION_FAILED",
            f"timebase={timebase!r} did not fail closed: {handshake}",
        )

    nonzero_subscribe = timed_handshake_events()
    nonzero_subscribe[0]["elapsed_ms"] = 1
    proc, handshake, _ = exercise(nonzero_subscribe)
    assert_true(proc.returncode == 3, proc.stdout + proc.stderr)
    assert_true(
        handshake["result"] == "COLLECTION_FAILED",
        f"nonzero subscription time passed: {handshake}",
    )

    decreasing = timed_handshake_events()
    decreasing[3]["elapsed_ms"] = int(decreasing[2]["elapsed_ms"]) - 1
    proc, handshake, _ = exercise(decreasing)
    assert_true(proc.returncode == 3, proc.stdout + proc.stderr)
    assert_true(handshake["result"] == "COLLECTION_FAILED", f"decreasing time passed: {handshake}")
    assert_true("elapsed_ms values decrease" in proc.stdout, proc.stdout)


def test_timed_handshake_state_drives_failure_taxonomy() -> None:
    def exercise(
        events: list[dict[str, object]],
        failure_kind: str,
    ) -> tuple[subprocess.CompletedProcess[str], dict]:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            write_window(root, "replay")
            replay = root / "replay"
            window_path = replay / "window_result.json"
            window = json.loads(window_path.read_text(encoding="utf-8"))
            window.update(
                {
                    "result": "RECONNECT_FAILED",
                    "duration_seconds": 300,
                    "reconnect_failure_kind": failure_kind,
                    "error": f"timed {failure_kind}",
                    "reconnect_preflight": {
                        **RECONNECT_PREFLIGHT_RESULT,
                        "handshake_ready_while_alive": False,
                        "cleanup_marker_count": 0,
                    },
                }
            )
            write_handshake_ledger(
                replay / "handshake_ledger_preflight.jsonl",
                events,
                schema_version=2,
            )
            write_reconnect_logs(replay, failure_kind=failure_kind)
            write_json(window_path, window)
            proc = run_score(root, "replay")
            result = json.loads((root / "bench_result.json").read_text(encoding="utf-8"))
            checks = result["windows"][0]["replay_checks"]["reconnect_checks"]
            return proc, checks["preflight_handshake_checks"]

    incomplete = timed_handshake_events((100, 1100))
    incomplete.pop(3)
    invalid = timed_handshake_events((100, 1099))

    proc, checks = exercise(incomplete, "handshake_timeout")
    assert_true(proc.returncode == 2, proc.stdout + proc.stderr)
    assert_true(checks["handshake_state"] == "incomplete", f"timeout state: {checks}")

    proc, checks = exercise(invalid, "handshake_invalid")
    assert_true(proc.returncode == 2, proc.stdout + proc.stderr)
    assert_true(checks["handshake_state"] == "invalid", f"invalid state: {checks}")

    for events, failure_kind in (
        (invalid, "handshake_timeout"),
        (incomplete, "handshake_invalid"),
    ):
        proc, checks = exercise(events, failure_kind)
        assert_true(proc.returncode == 3, proc.stdout + proc.stderr)
        assert_true(checks["result"] == "COLLECTION_FAILED", f"terminal mismatch: {checks}")
        assert_true("terminal contradicts its preflight ledger" in proc.stdout, proc.stdout)


def test_replay_reconnect_scores_two_ledgers_without_cross_credit() -> None:
    """Public behavior ID: V1-RECONNECT-SESSION-001."""
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        write_window(root, "replay")
        write_handshake_ledger(
            root / "replay" / "handshake_ledger_preflight.jsonl",
            canonical_handshake_events(request_channel="BAD4"),
        )
        proc = run_score(root, "replay")
        assert_true(proc.returncode == 0, proc.stdout + proc.stderr)
        result = json.loads((root / "bench_result.json").read_text(encoding="utf-8"))
        reconnect = result["windows"][0]["replay_checks"]["reconnect_checks"]
        assert_true(reconnect["result"] == "PASS", f"independent channels failed: {reconnect}")

    for ledger_name, checks_name in (
        ("handshake_ledger_preflight.jsonl", "preflight_handshake_checks"),
        ("handshake_ledger.jsonl", "primary_handshake_checks"),
    ):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            write_window(root, "replay")
            incomplete = canonical_handshake_events()
            incomplete.pop(6)
            write_handshake_ledger(root / "replay" / ledger_name, incomplete)
            proc = run_score(root, "replay")
            assert_true(proc.returncode == 2, f"{ledger_name}: {proc.stdout}{proc.stderr}")
            result = json.loads((root / "bench_result.json").read_text(encoding="utf-8"))
            reconnect = result["windows"][0]["replay_checks"]["reconnect_checks"]
            assert_true(
                reconnect[checks_name]["result"] == "FAIL",
                f"incomplete {ledger_name} passed: {reconnect}",
            )

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        write_window(root, "replay")
        preflight = canonical_handshake_events()
        preflight.pop(6)
        primary = canonical_handshake_events()
        primary.pop(4)
        write_handshake_ledger(
            root / "replay" / "handshake_ledger_preflight.jsonl",
            preflight,
        )
        write_handshake_ledger(root / "replay" / "handshake_ledger.jsonl", primary)
        proc = run_score(root, "replay")
        assert_true(proc.returncode == 2, proc.stdout + proc.stderr)
        result = json.loads((root / "bench_result.json").read_text(encoding="utf-8"))
        reconnect = result["windows"][0]["replay_checks"]["reconnect_checks"]
        assert_true(
            reconnect["preflight_handshake_checks"]["result"] == "FAIL",
            f"preflight received cross-ledger credit: {reconnect}",
        )
        assert_true(
            reconnect["primary_handshake_checks"]["result"] == "FAIL",
            f"primary received cross-ledger credit: {reconnect}",
        )


def test_replay_reconnect_rejects_extra_epochs_and_shared_artifacts() -> None:
    for ledger_name, checks_name in (
        ("handshake_ledger_preflight.jsonl", "preflight_handshake_checks"),
        ("handshake_ledger.jsonl", "primary_handshake_checks"),
    ):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            write_window(root, "replay")
            events = canonical_handshake_events()
            events.append({"event": "subscribe", "epoch": 2, "channel": "B2CE"})
            write_handshake_ledger(root / "replay" / ledger_name, events)
            proc = run_score(root, "replay")
            assert_true(proc.returncode == 2, f"{ledger_name}: {proc.stdout}{proc.stderr}")
            result = json.loads((root / "bench_result.json").read_text(encoding="utf-8"))
            reconnect = result["windows"][0]["replay_checks"]["reconnect_checks"]
            checks = reconnect[checks_name]
            assert_true(checks["epoch_count"] == 2, f"extra epoch was not observed: {checks}")
            assert_true(checks["result"] == "FAIL", f"extra epoch passed: {checks}")

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        write_window(root, "replay")
        window_path = root / "replay" / "window_result.json"
        window = json.loads(window_path.read_text(encoding="utf-8"))
        window["reconnect_preflight_handshake_ledger_path"] = "./handshake_ledger.jsonl"
        write_json(window_path, window)
        proc = run_score(root, "replay")
        assert_true(proc.returncode == 3, proc.stdout + proc.stderr)
        assert_true("handshake ledgers are not distinct" in proc.stdout, proc.stdout)
        result = json.loads((root / "bench_result.json").read_text(encoding="utf-8"))
        reconnect = result["windows"][0]["replay_checks"]["reconnect_checks"]
        assert_true(
            reconnect["artifact_checks"]["result"] == "COLLECTION_FAILED",
            f"shared artifact was not classified as collection failure: {reconnect}",
        )


def test_replay_reconnect_requires_preflight_artifact_and_exact_clear() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        write_window(root, "replay")
        window_path = root / "replay" / "window_result.json"
        window = json.loads(window_path.read_text(encoding="utf-8"))
        window.pop("reconnect_preflight_handshake_ledger_path")
        write_json(window_path, window)
        proc = run_score(root, "replay")
        assert_true(proc.returncode == 3, proc.stdout + proc.stderr)
        assert_true("missing required reconnect preflight handshake ledger path" in proc.stdout, proc.stdout)

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        write_window(root, "replay")
        ledger_path = root / "replay" / "handshake_ledger_preflight.jsonl"
        ledger_path.write_text("not-json\n", encoding="utf-8")
        proc = run_score(root, "replay")
        assert_true(proc.returncode == 3, proc.stdout + proc.stderr)
        assert_true("line 1 is not valid JSON" in proc.stdout, proc.stdout)

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        write_window(root, "replay")
        events = canonical_handshake_events()
        active_row = framed_bytes(
            0xD8,
            0xEA,
            0x43,
            [0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00],
        )
        events[2]["bytes"] = active_row
        write_handshake_ledger(
            root / "replay" / "handshake_ledger_preflight.jsonl",
            events,
        )
        proc = run_score(root, "replay")
        assert_true(proc.returncode == 2, proc.stdout + proc.stderr)
        assert_true("not the canonical clear row" in proc.stdout, proc.stdout)

        write_handshake_ledger(root / "replay" / "handshake_ledger_preflight.jsonl")
        write_handshake_ledger(root / "replay" / "handshake_ledger.jsonl", events)
        proc = run_score(root, "replay")
        assert_true(
            proc.returncode == 0,
            "primary handshake alert-row behavior changed: " + proc.stdout + proc.stderr,
        )


def test_replay_reconnect_requires_actionable_lifecycle_evidence() -> None:
    mutants = (
        ("handshake_ready_while_alive", False, "handshake_ready_while_alive=False expected=True"),
        ("serial_fence_observed", False, "serial_fence_observed=False expected=True"),
        ("managed_stop", False, "managed_stop=False expected=True"),
        ("confirmed_exit", False, "confirmed_exit=False expected=True"),
        ("serial_session_continuous", False, "serial_session_continuous=False expected=True"),
        (
            "boot_observed_before_second_complete",
            True,
            "boot_observed_before_second_complete=True expected=False",
        ),
        ("cleanup_marker_count", 0, "cleanup_marker_count=0 expected=1"),
        ("cleanup_marker_count", 2, "cleanup_marker_count=2 expected=1"),
    )
    for field, value, expected_message in mutants:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            write_window(root, "replay")
            window_path = root / "replay" / "window_result.json"
            window = json.loads(window_path.read_text(encoding="utf-8"))
            if value is None:
                window["reconnect_preflight"].pop(field)
            else:
                window["reconnect_preflight"][field] = value
            write_json(window_path, window)
            proc = run_score(root, "replay")
            assert_true(proc.returncode == 2, f"{field}: {proc.stdout}{proc.stderr}")
            assert_true(expected_message in proc.stdout, f"{field}: {proc.stdout}")

    for missing_field in ("serial_fence_observed", "cleanup_marker_count"):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            write_window(root, "replay")
            window_path = root / "replay" / "window_result.json"
            window = json.loads(window_path.read_text(encoding="utf-8"))
            window["reconnect_preflight"].pop(missing_field)
            write_json(window_path, window)
            proc = run_score(root, "replay")
            assert_true(proc.returncode == 3, f"{missing_field}: {proc.stdout}{proc.stderr}")
            assert_true("terminal result is missing fields" in proc.stdout, proc.stdout)

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        write_window(root, "replay")
        window_path = root / "replay" / "window_result.json"
        window = json.loads(window_path.read_text(encoding="utf-8"))
        window.pop("reconnect_preflight")
        write_json(window_path, window)
        proc = run_score(root, "replay")
        assert_true(proc.returncode == 3, proc.stdout + proc.stderr)
        assert_true("terminal result is missing" in proc.stdout, proc.stdout)

    for field, value in (
        ("confirmed_exit", "yes"),
        ("cleanup_marker_count", True),
    ):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            write_window(root, "replay")
            window_path = root / "replay" / "window_result.json"
            window = json.loads(window_path.read_text(encoding="utf-8"))
            window["reconnect_preflight"][field] = value
            write_json(window_path, window)
            proc = run_score(root, "replay")
            assert_true(proc.returncode == 3, f"{field}: {proc.stdout}{proc.stderr}")
            assert_true("terminal result has invalid fields" in proc.stdout, proc.stdout)


def test_pre_qstart_reconnect_failure_taxonomy_is_preserved() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        write_window(root, "replay")
        window_path = root / "replay" / "window_result.json"
        window = json.loads(window_path.read_text(encoding="utf-8"))
        window.update(
            {
                "result": "RECONNECT_FAILED",
                "duration_seconds": 300,
                "reconnect_failure_kind": "handshake_timeout",
                "error": "reconnect preflight timed out before one complete active handshake epoch",
                "reconnect_preflight": {
                    **RECONNECT_PREFLIGHT_RESULT,
                    "handshake_ready_while_alive": False,
                    "cleanup_marker_count": 0,
                },
            }
        )
        write_handshake_ledger(
            root / "replay" / "handshake_ledger_preflight.jsonl",
            canonical_handshake_events()[:5],
        )
        write_reconnect_logs(root / "replay", failure_kind="handshake_timeout")
        write_json(window_path, window)
        proc = run_score(root, "replay")
        assert_true(proc.returncode == 2, proc.stdout + proc.stderr)
        assert_true("handshake_timeout" in proc.stdout, proc.stdout)
        result = json.loads((root / "bench_result.json").read_text(encoding="utf-8"))
        reconnect = result["windows"][0]["replay_checks"]["reconnect_checks"]
        lifecycle = reconnect["lifecycle_checks"]
        assert_true(lifecycle["result"] == "PASS", f"timeout lifecycle failed: {lifecycle}")
        assert_true(
            lifecycle["diagnostics"]
            == ["replay reconnect cleanup was not collected after the early handshake terminal"],
            f"timeout cleanup was not diagnostic-only: {lifecycle}",
        )
        assert_true(
            not any(
                "handshake_ready_while_alive" in item or "cleanup_marker_count" in item
                for item in reconnect["evidence"]
            ),
            f"timeout retained derivative lifecycle evidence: {reconnect}",
        )

    for ledger_state in ("missing", "complete"):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            write_window(root, "replay")
            replay = root / "replay"
            window_path = replay / "window_result.json"
            window = json.loads(window_path.read_text(encoding="utf-8"))
            window.update(
                {
                    "result": "RECONNECT_FAILED",
                    "duration_seconds": 300,
                    "reconnect_failure_kind": "handshake_timeout",
                    "error": "timeout terminal",
                    "reconnect_preflight": {
                        **RECONNECT_PREFLIGHT_RESULT,
                        "handshake_ready_while_alive": False,
                        "cleanup_marker_count": 0,
                    },
                }
            )
            if ledger_state == "missing":
                (replay / "handshake_ledger_preflight.jsonl").unlink()
            write_reconnect_logs(replay, failure_kind="handshake_timeout")
            write_json(window_path, window)
            proc = run_score(root, "replay")
            assert_true(proc.returncode == 3, proc.stdout + proc.stderr)
            expected = "ledger is missing" if ledger_state == "missing" else "contradicts"
            assert_true(expected in proc.stdout, proc.stdout)

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        write_window(root, "replay")
        window_path = root / "replay" / "window_result.json"
        window = json.loads(window_path.read_text(encoding="utf-8"))
        window.update(
            {
                "result": "RECONNECT_FAILED",
                "duration_seconds": 300,
                "reconnect_failure_kind": "cleanup_missing",
                "error": "board did not report V1 disconnect cleanup after emulator exit",
                "reconnect_preflight": {
                    **RECONNECT_PREFLIGHT_RESULT,
                    "cleanup_marker_count": 0,
                },
            }
        )
        write_json(window_path, window)
        write_reconnect_logs(root / "replay", failure_kind="cleanup_missing")
        proc = run_score(root, "replay")
        assert_true(proc.returncode == 2, proc.stdout + proc.stderr)
        assert_true("cleanup_marker_count=0 expected=1" in proc.stdout, proc.stdout)

    for mutation, expected_text in (
        ("delayed_before_command", "requires one QSTATUS and one response"),
        ("duplicate_after_response", "requires one QSTATUS and one response"),
        ("malformed_response", "malformed QRESP evidence"),
    ):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            write_window(root, "replay")
            replay = root / "replay"
            window_path = replay / "window_result.json"
            window = json.loads(window_path.read_text(encoding="utf-8"))
            window.update(
                {
                    "result": "RECONNECT_FAILED",
                    "duration_seconds": 300,
                    "reconnect_failure_kind": "cleanup_missing",
                    "error": "board did not report V1 disconnect cleanup after emulator exit",
                    "reconnect_preflight": {
                        **RECONNECT_PREFLIGHT_RESULT,
                        "cleanup_marker_count": 0,
                    },
                }
            )
            write_reconnect_logs(replay, failure_kind="cleanup_missing")
            serial_path = replay / "bench_serial.log"
            lines = serial_path.read_text(encoding="utf-8").splitlines()
            boundary = lines.index("HOST_BOUNDARY reconnect_preflight_process_exited")
            command = lines.index(">>> QSTATUS", boundary + 1)
            response = lines.index(FENCE_RESPONSE, command + 1)
            if mutation == "delayed_before_command":
                lines.insert(command, FENCE_RESPONSE)
            elif mutation == "duplicate_after_response":
                lines.insert(response + 1, FENCE_RESPONSE)
            else:
                lines[response] = "QRESP {bad"
            serial_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
            write_json(window_path, window)
            proc = run_score(root, "replay")
            assert_true(proc.returncode == 3, f"{mutation}: {proc.stdout}{proc.stderr}")
            assert_true(expected_text in proc.stdout, f"{mutation}: {proc.stdout}")

    for failure_kind, cleanup_count in (
        ("active_session_lost", 1),
        ("cleanup_before_stop", 1),
        ("cleanup_count", 2),
    ):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            write_window(root, "replay")
            replay = root / "replay"
            window_path = replay / "window_result.json"
            window = json.loads(window_path.read_text(encoding="utf-8"))
            window.update(
                {
                    "result": "RECONNECT_FAILED",
                    "duration_seconds": 300,
                    "reconnect_failure_kind": failure_kind,
                    "error": failure_kind,
                    "reconnect_preflight": {
                        **RECONNECT_PREFLIGHT_RESULT,
                        "cleanup_marker_count": cleanup_count,
                    },
                }
            )
            write_reconnect_logs(replay, failure_kind=failure_kind)
            write_json(window_path, window)
            proc = run_score(root, "replay")
            assert_true(proc.returncode == 2, proc.stdout + proc.stderr)
            assert_true(failure_kind in proc.stdout, proc.stdout)

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        write_window(root, "replay")
        replay = root / "replay"
        window_path = replay / "window_result.json"
        window = json.loads(window_path.read_text(encoding="utf-8"))
        window.update(
            {
                "result": "RECONNECT_FAILED",
                "duration_seconds": 300,
                "reconnect_failure_kind": "handshake_invalid",
                "error": "wrong all-volume route",
                "reconnect_preflight": {
                    **RECONNECT_PREFLIGHT_RESULT,
                    "handshake_ready_while_alive": False,
                },
            }
        )
        events = canonical_handshake_events()
        events[-1]["channel"] = "B4E0"
        write_handshake_ledger(replay / "handshake_ledger_preflight.jsonl", events)
        # This mirrors the runner: semantic ledger validation fails before
        # handshake_ready and before the pre-stop fence, then process exit is
        # followed by a same-serial health fence.
        write_reconnect_logs(replay, failure_kind="handshake_invalid")
        write_json(window_path, window)
        proc = run_score(root, "replay")
        assert_true(proc.returncode == 2, proc.stdout + proc.stderr)
        assert_true("wrong all-volume route" in proc.stdout, proc.stdout)

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        write_window(root, "replay")
        replay = root / "replay"
        window_path = replay / "window_result.json"
        window = json.loads(window_path.read_text(encoding="utf-8"))
        window.update(
            {
                "result": "RECONNECT_FAILED",
                "duration_seconds": 300,
                "reconnect_failure_kind": "handshake_invalid",
                "error": "duplicate start request",
                "reconnect_preflight": {
                    **RECONNECT_PREFLIGHT_RESULT,
                    "handshake_ready_while_alive": False,
                    "cleanup_marker_count": 0,
                },
            }
        )
        events = canonical_handshake_events()
        events[2] = copy.deepcopy(events[1])
        write_handshake_ledger(replay / "handshake_ledger_preflight.jsonl", events)
        write_reconnect_logs(replay, failure_kind="handshake_invalid")
        write_json(window_path, window)
        proc = run_score(root, "replay")
        assert_true(proc.returncode == 2, proc.stdout + proc.stderr)
        result = json.loads((root / "bench_result.json").read_text(encoding="utf-8"))
        reconnect = result["windows"][0]["replay_checks"]["reconnect_checks"]
        lifecycle = reconnect["lifecycle_checks"]
        assert_true(lifecycle["result"] == "PASS", f"derived lifecycle failed: {lifecycle}")
        assert_true(
            lifecycle["diagnostics"]
            == ["replay reconnect cleanup was not collected after the early handshake terminal"],
            f"early-terminal cleanup diagnostic missing: {lifecycle}",
        )
        assert_true(
            not any(
                "handshake_ready_while_alive" in item or "cleanup_marker_count" in item
                for item in reconnect["evidence"]
            ),
            f"derived early-terminal fields remained gating evidence: {reconnect}",
        )
        assert_true(
            any("repeats start request" in item for item in reconnect["evidence"]),
            f"authoritative duplicate-start failure was lost: {reconnect}",
        )

        baseline_preflight = copy.deepcopy(window["reconnect_preflight"])
        serial_path = replay / "bench_serial.log"
        baseline_serial = serial_path.read_text(encoding="utf-8")

        def score_lifecycle(
            preflight: dict,
            cleanup_lines: int = 0,
        ) -> tuple[subprocess.CompletedProcess[str], dict]:
            lines = baseline_serial.splitlines()
            boundary = lines.index("HOST_BOUNDARY reconnect_preflight_process_exited")
            command = lines.index(">>> QSTATUS", boundary + 1)
            lines[command:command] = [
                f"[BLE] V1 disconnected; cleared LCD BLE state at {101 + index} ms"
                for index in range(cleanup_lines)
            ]
            serial_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
            window["reconnect_preflight"] = preflight
            write_json(window_path, window)
            scored = run_score(root, "replay")
            scored_result = json.loads(
                (root / "bench_result.json").read_text(encoding="utf-8")
            )
            scored_reconnect = scored_result["windows"][0]["replay_checks"][
                "reconnect_checks"
            ]
            return scored, scored_reconnect["lifecycle_checks"]

        one_cleanup = {**baseline_preflight, "cleanup_marker_count": 1}
        proc, lifecycle = score_lifecycle(one_cleanup, cleanup_lines=1)
        assert_true(proc.returncode == 2, proc.stdout + proc.stderr)
        assert_true(
            lifecycle == {"result": "PASS", "evidence": [], "diagnostics": []},
            f"observed early cleanup was not neutral: {lifecycle}",
        )

        two_cleanups = {**baseline_preflight, "cleanup_marker_count": 2}
        proc, lifecycle = score_lifecycle(two_cleanups, cleanup_lines=2)
        assert_true(proc.returncode == 2, proc.stdout + proc.stderr)
        assert_true(lifecycle["result"] == "FAIL", f"duplicate cleanup passed: {lifecycle}")
        assert_true(
            any(
                "cleanup_marker_count=2 expected=1" in item
                for item in lifecycle["evidence"]
            ),
            f"duplicate cleanup lost its lifecycle witness: {lifecycle}",
        )

        witness_mutants = (
            (
                "handshake_ready_while_alive",
                True,
                "handshake_ready_while_alive=True expected=False",
            ),
            ("serial_fence_observed", False, "serial_fence_observed=False expected=True"),
            ("managed_stop", False, "managed_stop=False expected=True"),
            ("confirmed_exit", False, "confirmed_exit=False expected=True"),
            (
                "serial_session_continuous",
                False,
                "serial_session_continuous=False expected=True",
            ),
            (
                "boot_observed_before_second_complete",
                True,
                "boot_observed_before_second_complete=True expected=False",
            ),
        )
        for field, value, expected in witness_mutants:
            mutant = {**baseline_preflight, field: value}
            proc, lifecycle = score_lifecycle(mutant)
            assert_true(proc.returncode == 2, f"{field}: {proc.stdout}{proc.stderr}")
            assert_true(lifecycle["result"] == "FAIL", f"{field} was weakened: {lifecycle}")
            assert_true(
                any(expected in item for item in lifecycle["evidence"]),
                f"{field}: {lifecycle}",
            )

        for field, value in (
            ("handshake_ready_while_alive", None),
            ("cleanup_marker_count", None),
            ("handshake_ready_while_alive", "yes"),
            ("cleanup_marker_count", True),
        ):
            mutant = copy.deepcopy(baseline_preflight)
            if value is None:
                mutant.pop(field)
            else:
                mutant[field] = value
            proc, lifecycle = score_lifecycle(mutant)
            assert_true(proc.returncode == 3, f"{field}: {proc.stdout}{proc.stderr}")
            assert_true(
                lifecycle["result"] == "COLLECTION_FAILED",
                f"invalid {field} did not fail closed: {lifecycle}",
            )

    for event_count in (0, 5, 6):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            write_window(root, "replay")
            replay = root / "replay"
            window_path = replay / "window_result.json"
            window = json.loads(window_path.read_text(encoding="utf-8"))
            window.update(
                {
                    "result": "RECONNECT_FAILED",
                    "duration_seconds": 300,
                    "reconnect_failure_kind": "handshake_invalid",
                    "error": "invalid terminal with incomplete ledger",
                    "reconnect_preflight": {
                        **RECONNECT_PREFLIGHT_RESULT,
                        "handshake_ready_while_alive": False,
                    },
                }
            )
            write_handshake_ledger(
                replay / "handshake_ledger_preflight.jsonl",
                canonical_handshake_events()[:event_count],
            )
            write_reconnect_logs(replay, failure_kind="handshake_invalid")
            write_json(window_path, window)
            proc = run_score(root, "replay")
            assert_true(proc.returncode == 3, proc.stdout + proc.stderr)
            assert_true(
                "invalid-handshake terminal contradicts" in proc.stdout,
                proc.stdout,
            )

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        write_window(root, "replay")
        replay = root / "replay"
        window_path = replay / "window_result.json"
        window = json.loads(window_path.read_text(encoding="utf-8"))
        window.update(
            {
                "result": "RECONNECT_FAILED",
                "duration_seconds": 300,
                "reconnect_failure_kind": "cleanup_missing",
                "error": "incomplete-ledger contradiction",
                "reconnect_preflight": {
                    **RECONNECT_PREFLIGHT_RESULT,
                    "cleanup_marker_count": 0,
                },
            }
        )
        write_handshake_ledger(
            replay / "handshake_ledger_preflight.jsonl",
            canonical_handshake_events()[:5],
        )
        write_reconnect_logs(replay, failure_kind="cleanup_missing")
        write_json(window_path, window)
        proc = run_score(root, "replay")
        assert_true(proc.returncode == 3, proc.stdout + proc.stderr)
        assert_true("requires a complete preflight handshake ledger" in proc.stdout, proc.stdout)

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        write_window(root, "replay")
        replay = root / "replay"
        window_path = replay / "window_result.json"
        window = json.loads(window_path.read_text(encoding="utf-8"))
        window.update(
            {
                "result": "RECONNECT_FAILED",
                "duration_seconds": 300,
                "reconnect_failure_kind": "cleanup_missing",
                "error": "failed preflight launched B",
                "reconnect_preflight": {
                    **RECONNECT_PREFLIGHT_RESULT,
                    "cleanup_marker_count": 0,
                },
            }
        )
        write_reconnect_logs(replay, failure_kind="cleanup_missing")
        serial = replay / "bench_serial.log"
        serial.write_text(
            serial.read_text(encoding="utf-8") + ">>> QSTART core 300\n",
            encoding="utf-8",
        )
        write_json(window_path, window)
        proc = run_score(root, "replay")
        assert_true(proc.returncode == 3, proc.stdout + proc.stderr)
        assert_true("started after a failed reconnect preflight" in proc.stdout, proc.stdout)

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        write_window(root, "replay")
        window_path = root / "replay" / "window_result.json"
        window = json.loads(window_path.read_text(encoding="utf-8"))
        window.update(
            {
                "result": "RECONNECT_FAILED",
                "duration_seconds": 300,
                "reconnect_failure_kind": "cleanup_missing",
                "error": "behavioral terminal with malformed ledger",
            }
        )
        write_json(window_path, window)
        (root / "replay" / "handshake_ledger_preflight.jsonl").write_text(
            "not-json\n", encoding="utf-8"
        )
        write_reconnect_logs(root / "replay", failure_kind="cleanup_missing")
        proc = run_score(root, "replay")
        assert_true(proc.returncode == 3, proc.stdout + proc.stderr)
        assert_true("line 1 is not valid JSON" in proc.stdout, proc.stdout)

    for message in ("V1 emulator exited early", "serial health confirmation failed"):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            write_window(root, "replay")
            window_path = root / "replay" / "window_result.json"
            window = json.loads(window_path.read_text(encoding="utf-8"))
            window.update({"result": "COLLECTION_FAILED", "error": message})
            write_json(window_path, window)
            proc = run_score(root, "replay")
            assert_true(proc.returncode == 3, proc.stdout + proc.stderr)
            assert_true(message in proc.stdout, proc.stdout)


def test_reconnect_raw_lifecycle_mutants_cannot_false_green() -> None:
    """Public behavior ID: V1-RECONNECT-SESSION-001."""

    def exercise(
        mutate: object,
        expected_exit: int,
        expected_text: str,
    ) -> dict[str, object]:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            write_window(root, "replay")
            replay = root / "replay"
            assert callable(mutate)
            mutate(replay)
            proc = run_score(root, "replay")
            assert_true(
                proc.returncode == expected_exit,
                f"expected {expected_exit}: {proc.stdout}{proc.stderr}",
            )
            assert_true(expected_text in proc.stdout, proc.stdout)
            return json.loads((root / "bench_result.json").read_text(encoding="utf-8"))

    def raw_console_diagnostics(result: dict[str, object]) -> list[str]:
        windows = result["windows"]
        assert isinstance(windows, list) and len(windows) == 1
        window = windows[0]
        assert isinstance(window, dict)
        replay_checks = window["replay_checks"]
        assert isinstance(replay_checks, dict)
        reconnect_checks = replay_checks["reconnect_checks"]
        assert isinstance(reconnect_checks, dict)
        raw_checks = reconnect_checks["raw_evidence_checks"]
        assert isinstance(raw_checks, dict)
        diagnostics = raw_checks["diagnostics"]
        assert isinstance(diagnostics, list)
        return [str(item) for item in diagnostics]

    def mutate_preflight_text(replay: Path, transform: object) -> None:
        path = replay / "v1replay_reconnect_preflight.log"
        assert callable(transform)
        path.write_text(transform(path.read_text(encoding="utf-8")), encoding="utf-8")

    def mutate_serial_lines(replay: Path, transform: object) -> None:
        path = replay / "bench_serial.log"
        lines = path.read_text(encoding="utf-8").splitlines()
        assert callable(transform)
        path.write_text("\n".join(transform(lines)) + "\n", encoding="utf-8")

    exercise(
        lambda replay: (replay / "v1replay_reconnect_preflight.log").unlink(),
        3,
        "reconnect preflight log is missing",
    )

    def wrong_owned_name(replay: Path) -> None:
        source = replay / "v1replay_reconnect_preflight.log"
        target = replay / "renamed_preflight.log"
        source.rename(target)
        window_path = replay / "window_result.json"
        window = json.loads(window_path.read_text(encoding="utf-8"))
        window["reconnect_preflight_log_path"] = str(target)
        write_json(window_path, window)

    exercise(wrong_owned_name, 3, "does not use its owned artifact name")

    def alias_raw_logs(replay: Path) -> None:
        preflight = replay / "v1replay_reconnect_preflight.log"
        preflight.unlink()
        os.link(replay / "bench_serial.log", preflight)

    exercise(alias_raw_logs, 3, "are not distinct artifacts")
    exercise(
        lambda replay: mutate_preflight_text(
            replay, lambda text: text + 'V1REPLAY_EVENT {"state":'
        ),
        3,
        "truncated machine event",
    )

    def transport_falls_after_ready(text: str) -> str:
        ready = 'V1REPLAY_EVENT {"state":"handshake_ready"}\n'
        return text.replace(
            ready,
            ready + 'V1REPLAY_EVENT {"state":"handshake_transport","active":false}\n',
            1,
        )

    exercise(
        lambda replay: mutate_preflight_text(replay, transport_falls_after_ready),
        2,
        "lost active transport before removal",
    )

    def negative_followed_by_malformed_transport(text: str) -> str:
        ready = 'V1REPLAY_EVENT {"state":"handshake_ready"}\n'
        return text.replace(
            ready,
            ready
            + 'V1REPLAY_EVENT {"state":"handshake_transport","active":false}\n'
            + 'V1REPLAY_EVENT {"state":"handshake_transport","active":"bad"}\n',
            1,
        )

    exercise(
        lambda replay: mutate_preflight_text(replay, negative_followed_by_malformed_transport),
        2,
        "lost active transport before removal",
    )

    def transport_becomes_active_after_ready(text: str) -> str:
        lines = text.splitlines()
        active = 'V1REPLAY_EVENT {"state":"handshake_transport","active":true}'
        ready = 'V1REPLAY_EVENT {"state":"handshake_ready"}'
        lines.remove(active)
        lines.insert(lines.index(ready) + 1, active)
        return "\n".join(lines) + "\n"

    result = exercise(
        lambda replay: mutate_preflight_text(replay, transport_becomes_active_after_ready),
        0,
        "bench result: PASS",
    )
    assert_true(
        any("published after readiness" in item for item in raw_console_diagnostics(result)),
        f"interleaved console order was not retained diagnostically: {result}",
    )

    def stale_negative_before_late_positive(text: str) -> str:
        active = 'V1REPLAY_EVENT {"state":"handshake_transport","active":true}\n'
        ready = 'V1REPLAY_EVENT {"state":"handshake_ready"}\n'
        without_early_positive = text.replace(active, "", 1)
        return without_early_positive.replace(
            ready,
            'V1REPLAY_EVENT {"state":"handshake_transport","active":false}\n'
            + ready
            + 'V1REPLAY_EVENT {"state":"handshake_transport","active":true}\n',
            1,
        )

    result = exercise(
        lambda replay: mutate_preflight_text(replay, stale_negative_before_late_positive),
        0,
        "bench result: PASS",
    )
    assert_true(
        any("published after readiness" in item for item in raw_console_diagnostics(result)),
        f"real race ordering was not retained diagnostically: {result}",
    )

    result = exercise(
        lambda replay: mutate_preflight_text(
            replay,
            lambda text: "\n".join(
                line
                for line in text.splitlines()
                if '"state":"handshake_transport"' not in line
            )
            + "\n",
        ),
        0,
        "bench result: PASS",
    )
    assert_true(
        any("missing transport events" in item for item in raw_console_diagnostics(result)),
        f"missing console telemetry was not retained diagnostically: {result}",
    )

    result = exercise(
        lambda replay: mutate_preflight_text(
            replay,
            lambda text: text.replace(
                'V1REPLAY_EVENT {"state":"handshake_ready"}\n',
                "",
                1,
            ),
        ),
        0,
        "bench result: PASS",
    )
    assert_true(
        any("missing its ready event" in item for item in raw_console_diagnostics(result)),
        f"missing ready telemetry was not retained diagnostically: {result}",
    )

    result = exercise(
        lambda replay: mutate_preflight_text(
            replay,
            lambda text: text.replace(
                'V1REPLAY_EVENT {"state":"handshake_ready"}\n',
                'V1REPLAY_EVENT {"state":"handshake_ready"}\n'
                'V1REPLAY_EVENT {"state":"handshake_ready"}\n',
                1,
            ),
        ),
        0,
        "bench result: PASS",
    )
    assert_true(
        any("repeats its ready event" in item for item in raw_console_diagnostics(result)),
        f"duplicate ready telemetry was not retained diagnostically: {result}",
    )

    result = exercise(
        lambda replay: mutate_preflight_text(
            replay,
            lambda text: text.replace(
                'V1REPLAY_EVENT {"state":"handshake_transport","active":true}\n',
                'V1REPLAY_EVENT {"state":"handshake_transport","active":"yes"}\n',
                1,
            ),
        ),
        0,
        "bench result: PASS",
    )
    assert_true(
        any("malformed transport event" in item for item in raw_console_diagnostics(result)),
        f"malformed transport telemetry was not retained diagnostically: {result}",
    )

    def configured_after_ready(text: str) -> str:
        lines = text.splitlines()
        configured = lines.pop(0)
        ready_index = next(i for i, line in enumerate(lines) if '"handshake_ready"' in line)
        lines.insert(ready_index + 1, configured)
        return "\n".join(lines) + "\n"

    result = exercise(
        lambda replay: mutate_preflight_text(replay, configured_after_ready),
        0,
        "bench result: PASS",
    )
    assert_true(
        any("configured event follows" in item for item in raw_console_diagnostics(result)),
        f"configured-event ordering was not retained diagnostically: {result}",
    )
    exercise(
        lambda replay: mutate_preflight_text(
            replay,
            lambda text: text
            + 'V1REPLAY_EVENT {"state":"replay_started"}\n',
        ),
        2,
        "entered the scored scenario",
    )
    exercise(
        lambda replay: mutate_preflight_text(
            replay,
            lambda text: text
            + "TX B2CE AA D8 EA 31 09 38 38 00 00 00 0C 0C 40 6E AB\n",
        ),
        2,
        "did not stay quiet",
    )
    result = exercise(
        lambda replay: mutate_preflight_text(
            replay,
            lambda text: text.replace(
                "TX B2CE " + " ".join(f"{byte:02X}" for byte in FIRST_ALERT_ROW) + "\n",
                "",
                1,
            ),
        ),
        0,
        "bench result: PASS",
    )
    assert_true(
        any("missing ledger-confirmed TX" in item for item in raw_console_diagnostics(result)),
        f"missing duplicate TX telemetry was not retained diagnostically: {result}",
    )

    def remove_line(lines: list[str], exact: str) -> list[str]:
        result = list(lines)
        result.remove(exact)
        return result

    def insert_after(lines: list[str], anchor: str, value: str) -> list[str]:
        result = list(lines)
        result.insert(result.index(anchor) + 1, value)
        return result

    exercise(
        lambda replay: mutate_serial_lines(
            replay,
            lambda lines: remove_line(lines, "HOST_BOUNDARY reconnect_preflight_start"),
        ),
        3,
        "missing one preflight-start",
    )

    def remove_barrier_command(lines: list[str]) -> list[str]:
        return [line for line in lines if line != f">>> QBSC08 {READINESS_NONCE}"]

    exercise(
        lambda replay: mutate_serial_lines(replay, remove_barrier_command),
        3,
        "exactly one QBSC08 nonce command",
    )
    exercise(
        lambda replay: mutate_serial_lines(
            replay,
            lambda lines: insert_after(
                lines,
                f">>> QBSC08 {READINESS_NONCE}",
                f">>> QBSC08 {READINESS_NONCE}",
            ),
        ),
        3,
        "exactly one QBSC08 nonce command",
    )
    exercise(
        lambda replay: mutate_serial_lines(
            replay,
            lambda lines: [
                ">>> QBSC08 uppercase-is-not-a-valid-nonce"
                if line == f">>> QBSC08 {READINESS_NONCE}"
                else line
                for line in lines
            ],
        ),
        3,
        "invalid nonce",
    )
    exercise(
        lambda replay: mutate_serial_lines(
            replay,
            lambda lines: [line for line in lines if line != READINESS_RESPONSE],
        ),
        3,
        "exactly one QBSC08 nonce response",
    )
    exercise(
        lambda replay: mutate_serial_lines(
            replay,
            lambda lines: insert_after(lines, READINESS_RESPONSE, READINESS_RESPONSE),
        ),
        3,
        "exactly one QBSC08 nonce response",
    )
    for invalid_response, expected in (
        ("QBSC08 {bad", "response is malformed"),
        (
            'QBSC08 {"schema":1,"nonce":"ffffffffffffffffffffffffffffffff","status":"ready"}',
            "does not match its command",
        ),
        (
            f'QBSC08 {{"schema":true,"nonce":"{READINESS_NONCE}","status":"ready"}}',
            "does not match its command",
        ),
        (
            f'QBSC08 {{"schema":1,"nonce":"{READINESS_NONCE}","status":"unknown"}}',
            "does not match its command",
        ),
    ):
        exercise(
            lambda replay, response=invalid_response: mutate_serial_lines(
                replay,
                lambda lines, response=response: [
                    response if line == READINESS_RESPONSE else line for line in lines
                ],
            ),
            3,
            expected,
        )

    def reorder_barrier_response(lines: list[str]) -> list[str]:
        result = list(lines)
        result.remove(READINESS_RESPONSE)
        result.insert(result.index(f">>> QBSC08 {READINESS_NONCE}"), READINESS_RESPONSE)
        return result

    exercise(
        lambda replay: mutate_serial_lines(replay, reorder_barrier_response),
        3,
        "response precedes its command",
    )

    for stray, expected in (
        ('QERR {"ok":false,"error":"stale"}', "transaction contains QERR"),
        (">>> QSTATUS", "transaction contains an unexpected command"),
    ):
        exercise(
            lambda replay, stray=stray: mutate_serial_lines(
                replay,
                lambda lines, stray=stray: insert_after(
                    lines,
                    f">>> QBSC08 {READINESS_NONCE}",
                    stray,
                ),
            ),
            3,
            expected,
        )

    def replace_final_readiness_response_with_stale(lines: list[str]) -> list[str]:
        result = list(lines)
        barrier = result.index(READINESS_RESPONSE)
        boundary = result.index("HOST_BOUNDARY reconnect_preflight_start")
        response = result.index(FENCE_RESPONSE, barrier + 1, boundary)
        result[response] = 'QRESP {"ok":true,"state":"running","suite":"core","mode":"current"}'
        return result

    exercise(
        lambda replay: mutate_serial_lines(
            replay, replace_final_readiness_response_with_stale
        ),
        3,
        "readiness replay reconnect serial fence returned a non-ready QRESP",
    )

    def insert_delayed_readiness_response(lines: list[str]) -> list[str]:
        result = list(lines)
        barrier_command = result.index(f">>> QBSC08 {READINESS_NONCE}")
        result.insert(barrier_command + 1, FENCE_RESPONSE)
        return result

    exercise(
        lambda replay: mutate_serial_lines(replay, insert_delayed_readiness_response),
        0,
        "bench result: PASS",
    )
    exercise(
        lambda replay: mutate_serial_lines(
            replay,
            lambda lines: remove_line(lines, "HOST_BOUNDARY reconnect_preflight_fence_begin"),
        ),
        3,
        "missing its pre-stop fence boundaries",
    )

    def remove_ready_and_pre_stop_fence(replay: Path) -> None:
        mutate_preflight_text(
            replay,
            lambda text: text.replace(
                'V1REPLAY_EVENT {"state":"handshake_ready"}\n',
                "",
                1,
            ),
        )
        mutate_serial_lines(
            replay,
            lambda lines: remove_line(
                lines,
                "HOST_BOUNDARY reconnect_preflight_fence_begin",
            ),
        )

    exercise(
        remove_ready_and_pre_stop_fence,
        3,
        "missing its pre-stop fence boundaries",
    )
    exercise(
        lambda replay: mutate_serial_lines(
            replay,
            lambda lines: insert_after(
                lines,
                "HOST_BOUNDARY reconnect_preflight_start",
                FENCE_RESPONSE,
            ),
        ),
        3,
        "unbounded pre-stop protocol exchange",
    )

    def insert_unbounded_pre_stop_exchange(lines: list[str]) -> list[str]:
        result = list(lines)
        anchor = result.index("HOST_BOUNDARY reconnect_preflight_start") + 1
        result[anchor:anchor] = [">>> QSTATUS", FENCE_RESPONSE]
        return result

    def remove_pre_stop_fence_response(lines: list[str]) -> list[str]:
        result = list(lines)
        begin = result.index("HOST_BOUNDARY reconnect_preflight_fence_begin")
        result.pop(result.index(FENCE_RESPONSE, begin + 1))
        return result

    exercise(
        lambda replay: mutate_serial_lines(replay, insert_unbounded_pre_stop_exchange),
        3,
        "unbounded pre-stop protocol exchange",
    )
    for boundary in (
        "HOST_BOUNDARY reconnect_post_cleanup_fence_begin",
        "HOST_BOUNDARY reconnect_post_cleanup_fence_complete",
        "HOST_BOUNDARY reconnect_pre_qstart_fence_begin",
        "HOST_BOUNDARY reconnect_pre_qstart_fence_complete",
    ):
        exercise(
            lambda replay, boundary=boundary: mutate_serial_lines(
                replay, lambda lines, boundary=boundary: remove_line(lines, boundary)
            ),
            3,
            "missing its two bounded post-cleanup fences",
        )

    exercise(
        lambda replay: mutate_serial_lines(
            replay,
            lambda lines: insert_after(
                lines,
                "HOST_BOUNDARY reconnect_preflight_start",
                "[BLE] V1 disconnected; cleared LCD BLE state at 99 ms",
            ),
        ),
        2,
        "cleanup marker occurred before managed process exit",
    )
    exercise(
        lambda replay: mutate_serial_lines(
            replay,
            lambda lines: insert_after(
                lines, "HOST_BOUNDARY reconnect_preflight_start", "BOOT bootId=2"
            ),
        ),
        3,
        "observed a board boot",
    )
    exercise(
        lambda replay: mutate_serial_lines(
            replay,
            remove_pre_stop_fence_response,
        ),
        3,
        "requires one QSTATUS and one response",
    )
    exercise(
        lambda replay: mutate_serial_lines(
            replay,
            lambda lines: [
                "QRESP {bad" if line == FENCE_RESPONSE else line
                for line in lines
            ],
        ),
        3,
        "malformed QRESP evidence",
    )
    for invalid_response in (
        'QRESP {"ok":true}',
        'QRESP {"ok":true,"state":"running","suite":"display","mode":"current"}',
    ):
        exercise(
            lambda replay, response=invalid_response: mutate_serial_lines(
                replay,
                lambda lines, response=response: [
                    response if line == FENCE_RESPONSE else line for line in lines
                ],
            ),
            3,
            "serial fence returned a non-ready QRESP",
        )
    for anchor in (
        "HOST_BOUNDARY reconnect_preflight_fence_begin",
        "[BLE] V1 disconnected; cleared LCD BLE state at 101 ms",
        "HOST_BOUNDARY reconnect_pre_qstart_fence_begin",
    ):
        def insert_fence_error(lines: list[str], anchor: str = anchor) -> list[str]:
            result = list(lines)
            command = result.index(">>> QSTATUS", result.index(anchor) + 1)
            result.insert(command + 1, 'QERR {"ok":false,"error":"not_ready"}')
            return result

        exercise(
            lambda replay, mutate=insert_fence_error: mutate_serial_lines(replay, mutate),
            3,
            "requires one QSTATUS and one response",
        )

        def insert_trailing_fence_error(lines: list[str], anchor: str = anchor) -> list[str]:
            result = list(lines)
            command = result.index(">>> QSTATUS", result.index(anchor) + 1)
            response = result.index(FENCE_RESPONSE, command + 1)
            result.insert(response + 1, 'QERR {"ok":false,"error":"stale"}')
            return result

        exercise(
            lambda replay, mutate=insert_trailing_fence_error: mutate_serial_lines(
                replay, mutate
            ),
            3,
            "requires one QSTATUS and one response",
        )
    exercise(
        lambda replay: mutate_serial_lines(
            replay,
            lambda lines: insert_after(
                lines,
                "HOST_BOUNDARY reconnect_post_cleanup_fence_complete",
                'QERR {"ok":false,"error":"stale"}',
            ),
        ),
        3,
        "unbounded pre-QSTART protocol exchange",
    )
    exercise(
        lambda replay: mutate_serial_lines(
            replay,
            lambda lines: [
                ">>> QSTART display 1" if line == ">>> QSTART core 300" else line
                for line in lines
            ],
        ),
        3,
        "wrong replacement QSTART command",
    )
    exercise(
        lambda replay: mutate_serial_lines(
            replay,
            lambda lines: [
                line.replace('"suite":"core"', '"suite":"display"')
                if line.startswith("QRESP ") and '"state":"running"' in line
                else line
                for line in lines
            ],
        ),
        3,
        "exactly one replacement running acknowledgement",
    )
    exercise(
        lambda replay: mutate_serial_lines(
            replay,
            lambda lines: insert_after(lines, ">>> QSTART core 300", ">>> QSTART core 300"),
        ),
        3,
        "repeated QSTART without one perf_sd_busy_retry",
    )
    exercise(
        lambda replay: mutate_serial_lines(
            replay,
            lambda lines: insert_after(
                lines,
                ">>> QSTART core 300",
                'QERR {"ok":false,"error":"fatal_ble"}',
            ),
        ),
        3,
        "unexpected QERR",
    )

    def duplicate_running_ack(lines: list[str]) -> list[str]:
        result = list(lines)
        ack = next(line for line in result if line.startswith("QRESP ") and '"running"' in line)
        result.insert(result.index(ack) + 1, ack)
        return result

    exercise(
        lambda replay: mutate_serial_lines(replay, duplicate_running_ack),
        3,
        "exactly one replacement running acknowledgement",
    )
    exercise(
        lambda replay: mutate_serial_lines(
            replay,
            lambda lines: lines
            + ['QEVENT {"ok":true,"state":"done","suite":"core","finalized":true}'],
        ),
        3,
        "exactly one replacement completion",
    )

    cleanup = "[BLE] V1 disconnected; cleared LCD BLE state at 101 ms"
    exercise(
        lambda replay: mutate_serial_lines(
            replay, lambda lines: remove_line(lines, cleanup)
        ),
        2,
        "exactly one cleanup marker",
    )
    exercise(
        lambda replay: mutate_serial_lines(
            replay, lambda lines: insert_after(lines, cleanup, cleanup)
        ),
        2,
        "exactly one cleanup marker",
    )
    exercise(
        lambda replay: mutate_serial_lines(
            replay,
            lambda lines: insert_after(
                lines,
                ">>> QSTART core 300",
                "[BLE] V1 disconnected; cleared LCD BLE state at 102 ms",
            ),
        ),
        2,
        "disconnected before completion",
    )

    exercise(
        lambda replay: mutate_preflight_text(
            replay,
            lambda text: text.replace("TX B2CE AA D6", "TX B2CE GG D6", 1),
        ),
        3,
        "malformed TX bytes",
    )

    def add_valid_retry(lines: list[str]) -> list[str]:
        result = list(lines)
        qstart = result.index(">>> QSTART core 300")
        result[qstart:qstart] = [
            ">>> QSTART core 300",
            'QERR {"ok":false,"message":"perf_sd_busy_retry"}',
        ]
        return result

    exercise(
        lambda replay: mutate_serial_lines(replay, add_valid_retry),
        0,
        "bench result: PASS",
    )

    def retry_with_conflicting_error(lines: list[str]) -> list[str]:
        result = add_valid_retry(lines)
        retry = result.index('QERR {"ok":false,"message":"perf_sd_busy_retry"}')
        result.insert(retry + 1, 'QERR {"ok":false,"error":"fatal_ble"}')
        return result

    exercise(
        lambda replay: mutate_serial_lines(replay, retry_with_conflicting_error),
        3,
        "unexpected QERR",
    )
    exercise(
        lambda replay: mutate_serial_lines(
            replay,
            lambda lines: insert_after(
                lines,
                'QRESP {"ok":true,"state":"running","suite":"core"}',
                'QEVENT {"ok":false,"state":"error","suite":"core"}',
            ),
        ),
        3,
        "failed replacement QEVENT",
    )
    for command in (">>> QABORT", ">>> QSTATUS", ">>> QGETCSV"):
        exercise(
            lambda replay, command=command: mutate_serial_lines(
                replay,
                lambda lines: insert_after(lines, ">>> QSTART core 300", command),
            ),
            3,
            "unexpected host command",
        )
    exercise(
        lambda replay: mutate_preflight_text(
            replay, lambda text: text + "\u001b[2K\rquiet status"
        ),
        0,
        "bench result: PASS",
    )


def test_replay_requires_a_readable_bounded_same_window_handshake_ledger() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        write_window(root, "replay")
        window_path = root / "replay" / "window_result.json"
        window = json.loads(window_path.read_text(encoding="utf-8"))
        window.pop("handshake_ledger_path")
        write_json(window_path, window)
        proc = run_score(root, "replay")
        assert_true(proc.returncode == 3, proc.stdout + proc.stderr)
        assert_true("missing required handshake ledger path" in proc.stdout, proc.stdout)

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        write_window(root, "replay")
        ledger_path = root / "replay" / "handshake_ledger.jsonl"
        ledger_path.write_bytes(b"\xff\xfe\xfd")
        proc = run_score(root, "replay")
        assert_true(proc.returncode == 3, proc.stdout + proc.stderr)
        assert_true("handshake ledger could not be read" in proc.stdout, proc.stdout)

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        write_window(root, "replay")
        outside = root / "other_window" / "handshake_ledger.jsonl"
        write_handshake_ledger(outside)
        window_path = root / "replay" / "window_result.json"
        window = json.loads(window_path.read_text(encoding="utf-8"))
        window["handshake_ledger_path"] = str(outside)
        write_json(window_path, window)
        proc = run_score(root, "replay")
        assert_true(proc.returncode == 3, proc.stdout + proc.stderr)
        assert_true("must resolve inside its replay window" in proc.stdout, proc.stdout)

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        write_window(root, "replay")
        ledger_path = root / "replay" / "handshake_ledger.jsonl"
        ledger_path.write_text("not-json\n", encoding="utf-8")
        proc = run_score(root, "replay")
        assert_true(proc.returncode == 3, proc.stdout + proc.stderr)
        assert_true("line 1 is not valid JSON" in proc.stdout, proc.stdout)

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        write_window(root, "replay")
        events = canonical_handshake_events()
        events[0]["timestamp"] = "private-or-wall-clock-data"
        write_handshake_ledger(root / "replay" / "handshake_ledger.jsonl", events)
        proc = run_score(root, "replay")
        assert_true(proc.returncode == 3, proc.stdout + proc.stderr)
        assert_true("invalid event schema" in proc.stdout, proc.stdout)

    for invalid_event in ([], {}):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            write_window(root, "replay")
            events = canonical_handshake_events()
            events[0]["event"] = invalid_event
            write_handshake_ledger(root / "replay" / "handshake_ledger.jsonl", events)
            proc = run_score(root, "replay")
            assert_true(proc.returncode == 3, proc.stdout + proc.stderr)
            assert_true("invalid event schema" in proc.stdout, proc.stdout)

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        write_window(root, "replay")
        repeated = [copy.deepcopy(canonical_handshake_events()[0]) for _ in range(13)]
        write_handshake_ledger(root / "replay" / "handshake_ledger.jsonl", repeated)
        proc = run_score(root, "replay")
        assert_true(proc.returncode == 3, proc.stdout + proc.stderr)
        assert_true("epoch has too many events" in proc.stdout, proc.stdout)


def test_replay_handshake_semantic_mutants_are_actionable_failures() -> None:
    mutants: list[tuple[str, list[dict[str, object]], str]] = []

    events = copy.deepcopy(canonical_handshake_events())
    events.pop(6)
    mutants.append(("omitted all-volume response", events, "no single epoch"))

    events = copy.deepcopy(canonical_handshake_events())
    events.pop(0)
    mutants.append(("omitted B2CE subscription", events, "start request occurred before B2CE subscription"))

    events = copy.deepcopy(canonical_handshake_events())
    events[6]["bytes"] = framed_bytes(0xD6, 0xEA, 0x3E, [0x04, 0x00, 0x04, 0x00])
    mutants.append(("wrong response ID", events, "wrong header, ID, length, or payload"))

    events = copy.deepcopy(canonical_handshake_events())
    events[4]["bytes"] = framed_bytes(0xD6, 0xEA, 0x02, list(b"v4.1039"))
    mutants.append(("wrong version payload", events, "wrong header, ID, length, or payload"))

    events = copy.deepcopy(canonical_handshake_events())
    wrong_length = list(ALL_VOLUME_RESPONSE)
    wrong_length[4] = 0x04
    wrong_length[-2] = sum(wrong_length[:-2]) & 0xFF
    events[6]["bytes"] = wrong_length
    mutants.append(("wrong response length", events, "invalid declared length"))

    events = copy.deepcopy(canonical_handshake_events())
    events[6]["bytes"] = framed_bytes(0xD6, 0xEA, 0x3D, [0x04, 0x04, 0x00, 0x00])
    mutants.append(("wrong volume field order", events, "wrong header, ID, length, or payload"))

    events = copy.deepcopy(canonical_handshake_events())
    events[6]["bytes"] = framed_bytes(0xD8, 0xEA, 0x3D, [0x04, 0x00, 0x04, 0x00])
    mutants.append(("wrong response header", events, "wrong header, ID, length, or payload"))

    events = copy.deepcopy(canonical_handshake_events())
    events[6]["channel"] = "B4E0"
    mutants.append(("wrong response route", events, "wrong response channel"))

    events = copy.deepcopy(canonical_handshake_events())
    events[3]["channel"] = "BAD4"
    mutants.append(("switched command channel", events, "switches its selected command channel"))

    events = copy.deepcopy(canonical_handshake_events())
    response = events.pop(6)
    events.insert(5, response)
    mutants.append(("response before request", events, "all-volume response occurred before its request"))

    events = copy.deepcopy(canonical_handshake_events())
    response = events.pop(4)
    events.insert(3, response)
    mutants.append(("version response before request", events, "version response occurred before its request"))

    events = copy.deepcopy(canonical_handshake_events())
    response = events.pop(6)
    events.append({"event": "subscribe", "epoch": 2, "channel": "B2CE"})
    response["epoch"] = 2
    events.append(response)
    mutants.append(("cross-epoch response", events, "all-volume response occurred before its request"))

    events = copy.deepcopy(canonical_handshake_events())
    events[6]["delivery"] = "enqueued"
    mutants.append(("enqueue only", events, "response was not delivered"))

    events = copy.deepcopy(canonical_handshake_events())
    stream = events.pop(2)
    events.insert(1, stream)
    mutants.append(("stream before start", events, "stream began before the accepted start request"))

    events = copy.deepcopy(canonical_handshake_events())
    version_request = events.pop(3)
    events.insert(1, version_request)
    mutants.append(("version request before start", events, "version request occurred before the accepted start request"))

    events = copy.deepcopy(canonical_handshake_events())
    all_volume_request = events.pop(5)
    events.insert(3, all_volume_request)
    mutants.append(("all-volume request before version request", events, "before version request"))

    events = copy.deepcopy(canonical_handshake_events())
    corrupt_checksum = list(ALL_VOLUME_RESPONSE)
    corrupt_checksum[-2] ^= 0x01
    events[6]["bytes"] = corrupt_checksum
    mutants.append(("wrong response checksum", events, "invalid checksum"))

    for label, events, expected_message in mutants:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            write_window(root, "replay")
            write_handshake_ledger(root / "replay" / "handshake_ledger.jsonl", events)
            proc = run_score(root, "replay")
            assert_true(proc.returncode == 2, f"{label}: {proc.stdout}{proc.stderr}")
            result = json.loads((root / "bench_result.json").read_text(encoding="utf-8"))
            handshake = result["windows"][0]["replay_checks"]["handshake_checks"]
            assert_true(handshake["result"] == "FAIL", f"{label}: {handshake}")
            assert_true(expected_message in proc.stdout, f"{label}: {proc.stdout}")


def test_replay_requires_a_readable_same_window_encounter_csv() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        write_window(root, "replay")
        window_path = root / "replay" / "window_result.json"
        window = json.loads(window_path.read_text(encoding="utf-8"))
        window.pop("encounter_csv_path")
        write_json(window_path, window)
        proc = run_score(root, "replay")
        assert_true(proc.returncode == 3, proc.stdout + proc.stderr)
        assert_true("missing required encounter CSV path" in proc.stdout, proc.stdout)

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        write_window(root, "replay")
        outside = root / "other_window" / "encounters.csv"
        write_encounter_csv(outside)
        window_path = root / "replay" / "window_result.json"
        window = json.loads(window_path.read_text(encoding="utf-8"))
        window["encounter_csv_path"] = str(outside)
        write_json(window_path, window)
        proc = run_score(root, "replay")
        assert_true(proc.returncode == 3, proc.stdout + proc.stderr)
        assert_true("must resolve inside its replay window" in proc.stdout, proc.stdout)

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        write_window(root, "replay")
        encounter_path = root / "replay" / "encounters_test.csv"
        encounter_path.write_bytes(b"\xff\xfe\xfd")
        proc = run_score(root, "replay")
        assert_true(proc.returncode == 3, proc.stdout + proc.stderr)
        assert_true("encounter CSV could not be read" in proc.stdout, proc.stdout)


def test_replay_encounter_artifact_and_logical_failures_are_classified() -> None:
    def assert_logical_failure(rows: list[dict[str, object]], expected_message: str) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            write_window(root, "replay")
            write_encounter_csv(root / "replay" / "encounters_test.csv", rows)
            proc = run_score(root, "replay")
            assert_true(proc.returncode == 2, proc.stdout + proc.stderr)
            assert_true(expected_message in proc.stdout, proc.stdout)

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        write_window(root, "replay")
        encounter_path = root / "replay" / "encounters_test.csv"
        columns = tuple(column for column in ENCOUNTER_COLUMNS if column != "rear_bars")
        write_encounter_csv(encounter_path, columns=columns)
        proc = run_score(root, "replay")
        assert_true(proc.returncode == 3, proc.stdout + proc.stderr)
        assert_true("missing required columns: rear_bars" in proc.stdout, proc.stdout)

    wrong_count = canonical_encounter_rows()
    for row in wrong_count:
        if row["encounter_id"] == 2 and row["sample_seq"] == 2:
            row["alert_count"] = 3
    assert_logical_failure(wrong_count, "cardinality does not match alert_count")

    inconsistent_event = canonical_encounter_rows()
    inconsistent_group = [
        row
        for row in inconsistent_event
        if row["encounter_id"] == 2 and row["sample_seq"] == 2
    ]
    inconsistent_group[1]["event"] = "END"
    assert_logical_failure(inconsistent_event, "rows disagree on event or alert_count")

    duplicate_index = canonical_encounter_rows()
    duplicate_group = [
        row
        for row in duplicate_index
        if row["encounter_id"] == 2 and row["sample_seq"] == 2
    ]
    duplicate_group[1]["v1_index"] = 1
    assert_logical_failure(duplicate_index, "ordered unique one-based v1_index")

    missing_priority = canonical_encounter_rows()
    for row in missing_priority:
        if row["encounter_id"] == 2 and row["sample_seq"] == 2:
            row["priority"] = 0
    assert_logical_failure(missing_priority, "requires exactly one priority row")

    dropped_snapshot = canonical_encounter_rows()
    dropped_snapshot[0]["dropped_snapshots"] = 1
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        write_window(root, "replay")
        write_encounter_csv(root / "replay" / "encounters_test.csv", dropped_snapshot)
        proc = run_score(root, "replay")
        assert_true(proc.returncode == 3, proc.stdout + proc.stderr)
        assert_true("reports dropped snapshots" in proc.stdout, proc.stdout)

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        write_window(root, "replay")
        encounter_path = root / "replay" / "encounters_test.csv"
        with encounter_path.open("a", encoding="utf-8") as handle:
            handle.write("1,2,3,SAMPLE,1\n")
        proc = run_score(root, "replay")
        assert_true(proc.returncode == 3, proc.stdout + proc.stderr)
        assert_true("truncated or has empty required fields" in proc.stdout, proc.stdout)


def test_replay_encounter_semantic_mutants_are_actionable_failures() -> None:
    semantic_fields = (
        "band",
        "frequency_mhz",
        "direction",
        "front_raw",
        "rear_raw",
        "front_bars",
        "rear_bars",
        "priority",
        "junk",
        "photo_type",
    )

    def assert_semantic_failure(rows: list[dict[str, object]], expected_message: str) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            write_window(root, "replay")
            write_encounter_csv(root / "replay" / "encounters_test.csv", rows)
            proc = run_score(root, "replay")
            assert_true(proc.returncode == 2, proc.stdout + proc.stderr)
            result = json.loads((root / "bench_result.json").read_text(encoding="utf-8"))
            encounter = result["windows"][0]["replay_checks"]["encounter_checks"]
            assert_true(encounter["result"] == "FAIL", f"unexpected encounter result: {encounter}")
            assert_true(expected_message in proc.stdout, proc.stdout)

    swapped = canonical_encounter_rows()
    swapped_group = [
        row
        for row in swapped
        if row["encounter_id"] == 2 and row["sample_seq"] == 2
    ]
    left = {field: swapped_group[0][field] for field in semantic_fields}
    right = {field: swapped_group[1][field] for field in semantic_fields}
    swapped_group[0].update(right)
    swapped_group[1].update(left)
    assert_semantic_failure(swapped, "first two-row state does not match")

    wrong_priority = canonical_encounter_rows()
    priority_group = [
        row
        for row in wrong_priority
        if row["encounter_id"] == 2 and row["sample_seq"] == 4
    ]
    priority_group[0]["priority"] = 1
    priority_group[1]["priority"] = 0
    assert_semantic_failure(wrong_priority, "unexpected or regressed active state")

    stale = canonical_encounter_rows()
    clear_down = [
        row
        for row in stale
        if row["encounter_id"] == 2 and row["sample_seq"] in {6, 7}
    ]
    for row in clear_down:
        row["alert_count"] = 3
    stale_source = next(
        row
        for row in stale
        if row["encounter_id"] == 2 and row["sample_seq"] == 4 and row["v1_index"] == 3
    )
    for sample_seq in (7, 6):
        group = [row for row in clear_down if row["sample_seq"] == sample_seq]
        stale_row = {
            **stale_source,
            "millis": group[0]["millis"],
            "sample_seq": sample_seq,
            "alert_count": 3,
        }
        insert_at = stale.index(group[-1]) + 1
        stale.insert(insert_at, stale_row)
    assert_semantic_failure(stale, "unexpected or regressed active state")

    split_across_encounters = canonical_encounter_rows()
    encounter_by_sequence = {
        4: 3,
        5: 3,
        6: 4,
        7: 4,
        8: 5,
        9: 5,
        10: 5,
    }
    for row in split_across_encounters:
        if row["encounter_id"] == 2 and row["sample_seq"] in encounter_by_sequence:
            row["encounter_id"] = encounter_by_sequence[int(row["sample_seq"])]
    assert_semantic_failure(split_across_encounters, "missing authored checkpoint")

    sample_only = [
        row
        for row in canonical_encounter_rows()
        if not (
            row["encounter_id"] == 2
            and row["sample_seq"] == 1
            and row["event"] == "START"
        )
    ]
    assert_semantic_failure(sample_only, "no START-led encounter")

    stale_after_final = canonical_encounter_rows()
    final_end = [
        row
        for row in stale_after_final
        if row["encounter_id"] == 2 and row["event"] == "END"
    ]
    for row in final_end:
        row["sample_seq"] = 11
    prior_three = [
        row
        for row in stale_after_final
        if row["encounter_id"] == 2 and row["sample_seq"] == 4
    ]
    regressed_rows = [
        {
            **row,
            "millis": int(final_end[0]["millis"]) - 1,
            "sample_seq": 10,
            "event": "SAMPLE",
        }
        for row in prior_three
    ]
    insert_at = stale_after_final.index(final_end[0])
    stale_after_final[insert_at:insert_at] = regressed_rows
    assert_semantic_failure(stale_after_final, "unexpected or regressed active state")

    wrong_end_state = canonical_encounter_rows()
    final_end_row = next(
        row
        for row in wrong_end_state
        if row["encounter_id"] == 2 and row["event"] == "END"
    )
    final_end_row.update(
        {
            "band": "Ka",
            "frequency_mhz": 34_700,
            "direction": "FRONT",
            "front_bars": 5,
            "rear_bars": 0,
        }
    )
    assert_semantic_failure(wrong_end_state, "END does not close the final authored one-row state")

    missing_end = [
        row
        for row in canonical_encounter_rows()
        if not (row["encounter_id"] == 2 and row["event"] == "END")
    ]
    assert_semantic_failure(missing_end, "no END after the authored active sequence")


def test_replay_scores_only_qstart_and_replacement_connection_sessions() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        write_window(root, "replay")
        csv_path = root / "replay" / "perf.csv"
        columns = [
            "millis",
            "prioritySelectRowFlag",
            "alertTablePublishes",
            "alertTablePublishes3Bogey",
            "prioritySelectFirstUsable",
            "prioritySelectFirstEntry",
            "prioritySelectAmbiguousIndex",
            "prioritySelectUnusableIndex",
            "prioritySelectInvalidChosen",
            "alertTableRowReplacements",
            "alertTableAssemblyTimeouts",
            "parserRowsBandNone",
            "parserRowsKuRaw",
            "displayLiveInvalidPrioritySkips",
            "displayLiveFallbackToUsable",
            "disc",
            "qDrop",
            "parseFail",
            "v1AllVolumeParsed",
        ]

        def row(
            millis: int,
            publishes: int,
            three: int,
            disc: int = 1,
            q_drop: int = 0,
            parse_fail: int = 0,
            all_volume: int | None = None,
        ) -> str:
            values = {column: 0 for column in columns}
            values.update(
                {
                    "millis": millis,
                    "prioritySelectRowFlag": publishes,
                    "alertTablePublishes": publishes,
                    "alertTablePublishes3Bogey": three,
                    "disc": disc,
                    "qDrop": q_drop,
                    "parseFail": parse_fail,
                    "v1AllVolumeParsed": (
                        1 if all_volume is None and millis >= 300_000 else (all_volume or 0)
                    ),
                }
            )
            return ",".join(str(values[column]) for column in columns)

        def write_sessions(
            sessions: list[list[str]],
            *,
            seqs: list[int] | None = None,
            boot_ids: list[int] | None = None,
            schemas: list[int] | None = None,
            uptimes: list[int] | None = None,
            tokens: list[str] | None = None,
        ) -> None:
            chunks: list[str] = []
            for seq, rows in enumerate(sessions, start=1):
                marker_seq = seqs[seq - 1] if seqs is not None else seq
                boot_id = boot_ids[seq - 1] if boot_ids is not None else 1
                schema = schemas[seq - 1] if schemas is not None else 46
                uptime = (
                    uptimes[seq - 1]
                    if uptimes is not None
                    else (int(rows[0].split(",", 1)[0]) if rows else seq)
                )
                token = tokens[seq - 1] if tokens is not None else f"S{seq}"
                chunks.extend(
                    [
                        ",".join(columns),
                        f"#session_start,seq={marker_seq},bootId={boot_id},uptime_ms={uptime},token={token},schema={schema}",
                        *rows,
                    ]
                )
            csv_path.write_text("\n".join(chunks) + "\n", encoding="utf-8")

        # Preflight owns the inherited disconnect. Exact authored counters may
        # span QSTART and the replacement connection, but the disconnect may not
        # change anywhere inside that selected two-session window.
        write_sessions(
            [
                [row(0, 0, 0, 0), row(30_000, 0, 0, 1, 1, 1)],
                [row(35_000, 0, 0, 1, 1, 1), row(40_000, 200, 30, 1, 1, 1)],
                [row(45_000, 200, 30, 1, 1, 1), row(300_000, 708, 30, 1, 1, 1)],
            ]
        )
        proc = run_score(root, "replay")
        assert_true(proc.returncode == 0, proc.stdout + proc.stderr)
        result = json.loads((root / "bench_result.json").read_text(encoding="utf-8"))
        checks = result["windows"][0]["replay_checks"]
        assert_true(checks["session_count"] == 2, f"wrong session scope: {checks}")
        assert_true(checks["total_nonempty_session_count"] == 3, f"wrong session scope: {checks}")
        assert_true(checks["session_indices"] == [2, 3], f"wrong session scope: {checks}")
        assert_true(checks["segment_scope"] == "replacement_window", f"wrong scope: {checks}")
        assert_true(checks["observed_deltas"]["disc"] == 0, f"wrong deltas: {checks}")
        assert_true(checks["observed_deltas"]["qDrop"] == 0, f"wrong deltas: {checks}")
        assert_true(checks["observed_deltas"]["parseFail"] == 0, f"wrong deltas: {checks}")
        assert_true(checks["observed_deltas"]["alertTablePublishes3Bogey"] == 30, f"wrong deltas: {checks}")

        for missing_column in ("disc", "alertTablePublishes"):
            replacement_columns = [
                column for column in columns if column != missing_column
            ]

            def without_missing_column(raw: str) -> str:
                return ",".join(
                    value
                    for column, value in zip(columns, raw.split(","))
                    if column != missing_column
                )

            csv_path.write_text(
                "\n".join(
                    [
                        ",".join(columns),
                        "#session_start,seq=1,bootId=1,uptime_ms=35000,token=QSTART,schema=46",
                        row(35_000, 0, 0),
                        row(40_000, 200, 30),
                        ",".join(replacement_columns),
                        "#session_start,seq=2,bootId=1,uptime_ms=45000,token=REPLACEMENT,schema=46",
                        without_missing_column(row(45_000, 200, 30)),
                        without_missing_column(row(300_000, 708, 30)),
                    ]
                )
                + "\n",
                encoding="utf-8",
            )
            proc = run_score(root, "replay")
            assert_true(proc.returncode == 3, proc.stdout + proc.stderr)
            assert_true(
                f"replay CSV is missing required columns: {missing_column}" in proc.stdout,
                proc.stdout,
            )

        write_sessions(
            [
                [row(0, 0, 0, 0), row(30_000, 0, 0, 1)],
                [row(35_000, 0, 0), row(40_000, 200, 30)],
                [row(45_000, 200, 30), row(300_000, 708, 30, 2)],
            ]
        )
        proc = run_score(root, "replay")
        assert_true(proc.returncode == 2, proc.stdout + proc.stderr)
        assert_true("disc delta=1 expected=0" in proc.stdout, proc.stdout)

        write_sessions(
            [
                [row(0, 0, 0, 0)],
                [row(35_000, 0, 0), row(40_000, 200, 30, 1, 1)],
                [row(45_000, 200, 30, 1, 1), row(300_000, 708, 30, 1, 1, 2)],
            ]
        )
        proc = run_score(root, "replay")
        assert_true(proc.returncode == 2, proc.stdout + proc.stderr)
        assert_true("parseFail delta=2 expected=0" in proc.stdout, proc.stdout)

        # Counters earned by preflight cannot make a deficient replacement pass.
        write_sessions(
            [
                [row(0, 0, 0, 0), row(30_000, 708, 30, 1)],
                [row(35_000, 708, 30), row(40_000, 708, 30)],
                [row(45_000, 708, 30), row(300_000, 708, 30)],
            ]
        )
        proc = run_score(root, "replay")
        assert_true(proc.returncode == 2, proc.stdout + proc.stderr)
        assert_true("alertTablePublishes delta=0 expected=708" in proc.stdout, proc.stdout)

        write_sessions([[row(35_000, 0, 0), row(300_000, 708, 30)]])
        proc = run_score(root, "replay")
        assert_true(proc.returncode == 3, proc.stdout + proc.stderr)
        assert_true("requires nonempty QSTART and replacement-connection sessions" in proc.stdout, proc.stdout)

        write_sessions(
            [
                [row(35_000, 0, 0), row(40_000, 200, 30)],
                [row(45_000, 200, 30), row(300_000, 708, 30)],
            ],
            seqs=[3, 5],
        )
        proc = run_score(root, "replay")
        assert_true(proc.returncode == 3, proc.stdout + proc.stderr)
        assert_true("invalid or discontinuous metadata" in proc.stdout, proc.stdout)

        for metadata in (
            {"boot_ids": [1, 2]},
            {"schemas": [46, 0]},
            {"schemas": [46, 47]},
            {"uptimes": [10, 10]},
            {"tokens": ["SAME", "SAME"]},
            {"tokens": ["S1", ""]},
            {"seqs": [4, 3]},
        ):
            write_sessions(
                [
                    [row(35_000, 0, 0), row(40_000, 200, 30)],
                    [row(45_000, 200, 30), row(300_000, 708, 30)],
                ],
                **metadata,
            )
            proc = run_score(root, "replay")
            assert_true(proc.returncode == 3, proc.stdout + proc.stderr)
            assert_true("invalid or discontinuous metadata" in proc.stdout, proc.stdout)

        write_sessions(
            [
                [row(35_000, 0, 0), row(40_000, 200, 30)],
                [row(45_000, 200, 30), row(300_000, 708, 30)],
            ],
            uptimes=[35_000, 40_000],
        )
        proc = run_score(root, "replay")
        assert_true(proc.returncode == 3, proc.stdout + proc.stderr)
        assert_true("replacement marker does not follow the QSTART baseline" in proc.stdout, proc.stdout)

        for replacement_rows, uptimes in (
            ([row(45_000, 200, 30), row(300_000, 708, 30)], [35_000, 46_000]),
            (
                [row(45_000, 200, 30), row(44_000, 200, 30), row(300_000, 708, 30)],
                [35_000, 45_000],
            ),
        ):
            write_sessions(
                [
                    [row(35_000, 0, 0), row(40_000, 200, 30)],
                    replacement_rows,
                ],
                uptimes=uptimes,
            )
            proc = run_score(root, "replay")
            assert_true(proc.returncode == 3, proc.stdout + proc.stderr)
            assert_true("invalid or regressed row timing" in proc.stdout, proc.stdout)

        write_sessions(
            [
                [row(0, 0, 0, 0), row(30_000, 0, 0, 1)],
                [row(35_000, 0, 0), row(40_000, 400, 30)],
                [row(45_000, 200, 30), row(300_000, 708, 30)],
            ]
        )
        proc = run_score(root, "replay")
        assert_true(proc.returncode == 3, proc.stdout + proc.stderr)
        assert_true("cumulative counters regress" in proc.stdout, proc.stdout)

        write_sessions(
            [
                [row(0, 0, 0, 0), row(30_000, 0, 0, 1)],
                [row(35_000, 0, 0), row(40_000, 200, 30)],
                [row(45_000, 200, 30), row(300_000, 708, 30)],
            ]
        )
        lines = csv_path.read_text(encoding="utf-8").splitlines()
        replacement_first = lines.index(row(45_000, 200, 30))
        lines.insert(
            replacement_first + 1,
            "#session_start,seq=3,bootId=1,uptime_ms=45000,token=INJECTED,schema=46",
        )
        csv_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
        proc = run_score(root, "replay")
        assert_true(proc.returncode == 3, proc.stdout + proc.stderr)
        assert_true("final replacement session marker is missing or invalid" in proc.stdout, proc.stdout)

        # An empty replacement marker is not usable evidence; load_sessions
        # intentionally omits it, leaving only one nonempty session.
        write_sessions([[row(35_000, 0, 0), row(300_000, 708, 30)], []])
        proc = run_score(root, "replay")
        assert_true(proc.returncode == 3, proc.stdout + proc.stderr)
        assert_true("requires nonempty QSTART and replacement-connection sessions" in proc.stdout, proc.stdout)

        # Even with two earlier nonempty sessions, a trailing empty B marker
        # cannot be silently dropped and replaced by preflight evidence.
        write_sessions(
            [
                [row(0, 0, 0, 0), row(30_000, 0, 0, 1)],
                [row(35_000, 0, 0), row(300_000, 708, 30)],
                [],
            ]
        )
        proc = run_score(root, "replay")
        assert_true(proc.returncode == 3, proc.stdout + proc.stderr)
        assert_true("final replacement session marker is missing or invalid" in proc.stdout, proc.stdout)

        write_sessions(
            [
                [row(35_000, 0, 0), row(40_000, 200, 30)],
                [row(45_000, 200, 30), row(300_000, 708, 30)],
                [],
            ],
            seqs=[1, 2, 2],
        )
        proc = run_score(root, "replay")
        assert_true(proc.returncode == 3, proc.stdout + proc.stderr)
        assert_true("final replacement session marker is missing or invalid" in proc.stdout, proc.stdout)


def test_replay_mismatch_is_actionable_failure() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        write_window(root, "replay", replay_publishes=707)
        proc = run_score(root, "replay")
        assert_true(proc.returncode == 2, proc.stdout + proc.stderr)
        assert_true("alertTablePublishes delta=707 expected=708" in proc.stdout, proc.stdout)


def test_replay_process_failure_is_collection_failure() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        write_window(root, "replay", replay_completed=False)
        proc = run_score(root, "replay")
        assert_true(proc.returncode == 3, proc.stdout + proc.stderr)
        assert_true("v1replay did not complete successfully" in proc.stdout, proc.stdout)


def test_managed_emulator_must_cover_every_live_window() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        write_window(root, "core")
        window_path = root / "core" / "window_result.json"
        window = json.loads(window_path.read_text(encoding="utf-8"))
        window["v1_emulator"] = {
            "mode": "idle",
            "completed": False,
            "managed_stop": True,
        }
        write_json(window_path, window)
        proc = run_score(root, "core")
        assert_true(proc.returncode == 3, proc.stdout + proc.stderr)
        assert_true("managed V1 emulator did not cover" in proc.stdout, proc.stdout)


def test_requested_replay_camera_separates_product_and_evidence_failures() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        write_window(root, "replay", camera_result="CAPTURED")
        proc = run_score(root, "replay", camera_suites=("replay",))
        assert_true(proc.returncode == 0, proc.stdout + proc.stderr)
        assert_true("camera evidence: PASS (only replay is gated)" in proc.stdout, proc.stdout)
        assert_true("camera=PASS (gated replay validator)" in proc.stdout, proc.stdout)

        write_window(root, "replay", camera_result="CAPTURED", camera_grade_result="FAIL")
        proc = run_score(root, "replay", camera_suites=("replay",))
        assert_true(proc.returncode == 2, proc.stdout + proc.stderr)
        assert_true("replay camera evidence disagrees" in proc.stdout, proc.stdout)

        write_window(
            root,
            "replay",
            camera_result="CAPTURED",
            camera_grade_result="INCONCLUSIVE",
            camera_grade_errors=("camera registration failed",),
        )
        proc = run_score(root, "replay", camera_suites=("replay",))
        assert_true(proc.returncode == 3, proc.stdout + proc.stderr)
        assert_true("bench result: EVIDENCE_FAILED" in proc.stdout, proc.stdout)
        assert_true("collection: PASS" in proc.stdout, proc.stdout)
        assert_true("camera evidence: INCONCLUSIVE" in proc.stdout, proc.stdout)
        assert_true("replay camera evidence is inconclusive" in proc.stdout, proc.stdout)

        write_window(
            root,
            "replay",
            hard=1,
            camera_result="CAPTURED",
            camera_grade_result="INCONCLUSIVE",
            camera_grade_errors=("camera registration failed",),
        )
        proc = run_score(root, "replay", camera_suites=("replay",))
        assert_true(proc.returncode == 2, proc.stdout + proc.stderr)
        assert_true("bench result: FAIL" in proc.stdout, proc.stdout)
        assert_true("camera evidence: INCONCLUSIVE" in proc.stdout, proc.stdout)

        write_window(root, "replay", camera_result="CAPTURED")
        (root / "replay" / "camera" / "grades" / f"{CURRENT_GRADER_FINGERPRINT}.json").unlink()
        proc = run_score(root, "replay", camera_suites=("replay",))
        assert_true(proc.returncode == 3, proc.stdout + proc.stderr)
        assert_true("bench result: EVIDENCE_FAILED" in proc.stdout, proc.stdout)
        assert_true("has no current-fingerprint mechanical grade" in proc.stdout, proc.stdout)

        write_window(root, "replay", camera_result="CAPTURED")
        (root / "replay" / "camera" / "capture_manifest.json").unlink()
        (root / "replay" / "camera" / "camera_result.json").unlink()
        proc = run_score(root, "replay", camera_suites=("replay",))
        assert_true(proc.returncode == 3, proc.stdout + proc.stderr)
        assert_true("legacy ownership" in proc.stdout, proc.stdout)

        write_window(root, "replay", camera_result="CAPTURE_FAILED")
        proc = run_score(root, "replay", camera_suites=("replay",))
        assert_true(proc.returncode == 3, proc.stdout + proc.stderr)
        assert_true("was not captured: camera unavailable" in proc.stdout, proc.stdout)
        assert_true("legacy ownership" not in proc.stdout, proc.stdout)


def test_strict_camera_ownership_and_confidence_are_required() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        grade_path = root / "replay" / "camera" / "grades" / f"{CURRENT_GRADER_FINGERPRINT}.json"

        write_window(root, "replay", camera_result="CAPTURED")
        grade = json.loads(grade_path.read_text(encoding="utf-8"))
        grade["capture_id"] = "f" * 64
        write_json(grade_path, grade)
        proc = run_score(root, "replay", camera_suites=("replay",))
        assert_true(proc.returncode == 3, proc.stdout + proc.stderr)
        assert_true("ownership could not be verified" in proc.stdout, proc.stdout)

        write_window(root, "replay", camera_result="CAPTURED")
        grade = json.loads(grade_path.read_text(encoding="utf-8"))
        grade["grader_fingerprint"] = "e" * 64
        write_json(grade_path, grade)
        proc = run_score(root, "replay", camera_suites=("replay",))
        assert_true(proc.returncode == 3, proc.stdout + proc.stderr)
        assert_true("ownership could not be verified" in proc.stdout, proc.stdout)

        write_window(root, "replay", camera_result="CAPTURED", camera_grade_result="FAIL")
        grade = json.loads(grade_path.read_text(encoding="utf-8"))
        grade["confidence"] = {"result": "INCONCLUSIVE", "gates": {}}
        write_json(grade_path, grade)
        proc = run_score(root, "replay", camera_suites=("replay",))
        assert_true(proc.returncode == 3, proc.stdout + proc.stderr)
        assert_true("FAIL lacks passed confidence" in proc.stdout, proc.stdout)

        write_window(root, "replay", camera_result="CAPTURED")
        (root / "replay" / "camera" / "capture_manifest.json").unlink()
        grade_path.unlink()
        write_json(
            root / "replay" / "camera" / "camera_grade.json",
            {"schema_version": 1, "kind": "bench_camera_grade", "result": "PASS"},
        )
        proc = run_score(root, "replay", camera_suites=("replay",))
        assert_true(proc.returncode == 3, proc.stdout + proc.stderr)
        assert_true("legacy ownership" in proc.stdout, proc.stdout)


def test_strict_camera_bytes_and_window_identity_are_required() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        write_window(root, "replay", camera_result="CAPTURED")
        video = root / "replay" / "camera" / "evidence_exp50.mov"
        video.write_bytes(b"tampered")
        proc = run_score(root, "replay", camera_suites=("replay",))
        assert_true(proc.returncode == 3, proc.stdout + proc.stderr)
        assert_true("input hash changed: video" in proc.stdout, proc.stdout)

        for field, mismatch in (
            ("product_fingerprint", "f" * 64),
            ("scenario_fingerprint", "e" * 64),
        ):
            write_window(root, "replay", camera_result="CAPTURED")
            window_path = root / "replay" / "window_result.json"
            window = json.loads(window_path.read_text(encoding="utf-8"))
            window[field] = mismatch
            write_json(window_path, window)
            proc = run_score(root, "replay", camera_suites=("replay",))
            assert_true(proc.returncode == 3, proc.stdout + proc.stderr)
            assert_true(
                f"window and performance manifest {field} identities do not agree" in proc.stdout,
                proc.stdout,
            )

            write_window(root, "replay", camera_result="CAPTURED")
            window = json.loads(window_path.read_text(encoding="utf-8"))
            window[field] = mismatch
            write_json(window_path, window)
            manifest_path = root / "replay" / "manifest.json"
            performance_manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
            performance_manifest[field] = mismatch
            write_json(manifest_path, performance_manifest)
            proc = run_score(root, "replay", camera_suites=("replay",))
            assert_true(proc.returncode == 3, proc.stdout + proc.stderr)
            assert_true(f"{field} does not match the current window" in proc.stdout, proc.stdout)


def test_only_replay_camera_grade_is_required_by_the_full_bench() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        write_window(root, "core", camera_result="CAPTURED", camera_grade_result="FAIL")
        write_window(root, "display", camera_result="CAPTURED", camera_grade_result="FAIL")
        write_window(root, "replay", camera_result="CAPTURED", camera_grade_result="PASS")
        proc = run_score(
            root,
            "core",
            "display",
            "replay",
            camera_suites=("replay",),
        )
        assert_true(proc.returncode == 0, proc.stdout + proc.stderr)
        assert_true(
            "core: PASS" in proc.stdout and "camera=CAPTURED (diagnostic only)" in proc.stdout,
            proc.stdout,
        )
        assert_true(
            "display: PASS" in proc.stdout and "camera=CAPTURED (exercise only)" in proc.stdout,
            proc.stdout,
        )
        assert_true(
            "replay: PASS" in proc.stdout and "camera=PASS (gated replay validator)" in proc.stdout,
            proc.stdout,
        )


def test_camera_preflight_evidence_failure_needs_no_metrics_artifacts() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        replay = root / "replay"
        preflight = {
            "schema_version": 1,
            "kind": "bench_camera_preflight",
            "result": "INCONCLUSIVE",
            "diagnostics": [
                {
                    "code": "screen_landmark_unreadable",
                    "message": "camera landmark lacks readable SCAN glyph structure",
                    "measured": {"fill_ratio": 1.0, "internal_gap_runs": 0},
                    "thresholds": {"fill_ratio": [0.1, 0.72], "minimum_internal_gap_runs": 2},
                }
            ],
        }
        write_json(replay / "camera" / "camera_preflight.json", preflight)
        write_json(
            replay / "window_result.json",
            {
                "schema_version": 1,
                "result": "EVIDENCE_FAILED",
                "suite": "replay",
                "git_sha": FULL_SHA,
                "git_ref": "dev/test",
                "git_worktree_clean": True,
                "product_fingerprint": "a" * 64,
                "grader_fingerprint": CURRENT_GRADER_FINGERPRINT,
                "scenario_fingerprint": "b" * 64,
                "camera": {
                    "result": "CAPTURE_FAILED",
                    "preflight": "camera_preflight.json",
                    "preflight_result": "INCONCLUSIVE",
                    "preflight_diagnostics": preflight["diagnostics"],
                },
            },
        )
        proc = run_score(root, "replay", camera_suites=("replay",))
        assert_true(proc.returncode == 3, proc.stdout + proc.stderr)
        assert_true("bench result: EVIDENCE_FAILED" in proc.stdout, proc.stdout)
        assert_true("collection: PASS" in proc.stdout, proc.stdout)
        assert_true("screen_landmark_unreadable" in proc.stdout, proc.stdout)
        assert_true("bench result: FAIL" not in proc.stdout, proc.stdout)


def main() -> int:
    test_no_baseline_language_does_not_make_bench_fail()
    test_baseline_only_regression_is_comparison_not_verdict()
    test_required_missing_metric_remains_a_hard_failure()
    test_failed_base_result_remains_a_hard_failure()
    test_core_metric_failure_is_actionable_failure()
    test_display_metric_failure_is_actionable_failure()
    test_advisory_absolute_bound_is_a_warning()
    test_custom_output_preserves_canonical_summary_pair()
    test_missing_window_artifact_is_collection_failure()
    test_replay_exact_invariants_are_part_of_the_verdict()
    test_replay_all_volume_consumption_mutants_are_fail_closed()
    test_replay_handshake_accepts_independent_reply_order_and_complete_reconnect_epoch()
    test_timed_handshake_start_retry_contract_is_fail_closed()
    test_timed_handshake_state_drives_failure_taxonomy()
    test_replay_reconnect_scores_two_ledgers_without_cross_credit()
    test_replay_reconnect_rejects_extra_epochs_and_shared_artifacts()
    test_replay_reconnect_requires_preflight_artifact_and_exact_clear()
    test_replay_reconnect_requires_actionable_lifecycle_evidence()
    test_pre_qstart_reconnect_failure_taxonomy_is_preserved()
    test_reconnect_raw_lifecycle_mutants_cannot_false_green()
    test_replay_requires_a_readable_bounded_same_window_handshake_ledger()
    test_replay_handshake_semantic_mutants_are_actionable_failures()
    test_replay_requires_a_readable_same_window_encounter_csv()
    test_replay_encounter_artifact_and_logical_failures_are_classified()
    test_replay_encounter_semantic_mutants_are_actionable_failures()
    test_replay_scores_only_qstart_and_replacement_connection_sessions()
    test_replay_mismatch_is_actionable_failure()
    test_replay_process_failure_is_collection_failure()
    test_managed_emulator_must_cover_every_live_window()
    test_requested_replay_camera_separates_product_and_evidence_failures()
    test_strict_camera_ownership_and_confidence_are_required()
    test_strict_camera_bytes_and_window_identity_are_required()
    test_only_replay_camera_grade_is_required_by_the_full_bench()
    test_camera_preflight_evidence_failure_needs_no_metrics_artifacts()
    print("bench scorer tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
