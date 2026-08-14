#!/usr/bin/env python3
"""Run repeated managed V1 handshake/disconnect cycles on one HIL board.

This is a focused reconnect stress screen.  It deliberately does not start a
firmware metrics window, collect CSV data, or open a camera.  Each cycle owns a
fresh handshake-only emulator process and schema-v2 ledger, then grades that
ledger with the same decoder used by ``tools/bench_score.py``.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import time
from typing import Any, Mapping
import uuid


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_ARTIFACT_ROOT = ROOT / ".artifacts" / "bench" / "reconnect_stress"
DEFAULT_BOARD_ID = "release"
DEFAULT_TIMEOUT_SECONDS = 45.0
MAX_CYCLES = 1000

# In handshake-only stress mode the clear row is released as soon as the
# second accepted START arrives.  This value is only the bounded fallback if
# that trigger never arrives; 1999 ms is v1replay's CLI maximum and expires
# just before a third slot at the firmware's documented >=1 s retry cadence.
SECOND_START_RELEASE_SAFETY_DEADLINE_MS = 1999
# Keep the emulator alive long enough to expose another retry at the documented
# >=1 s cadence after clear delivery.  The final ledger grade also covers the
# bounded pre-stop fence that follows this observation.
POST_READY_OBSERVATION_SECONDS = 1.1
PRE_STOP_FENCE_TIMEOUT_SECONDS = 0.25
FIRST_CYCLE_WAIT_SECONDS = 2.1
INTER_CYCLE_WAIT_SECONDS = 1.1

LEDGER_NAME = "handshake_ledger_preflight.jsonl"
EMULATOR_LOG_NAME = "v1replay_reconnect_preflight.log"
SERIAL_LOG_NAME = "bench_serial.log"
SERIAL_SLICE_NAME = "bench_serial_slice.log"
RESULT_NAME = "stress_result.json"
SUMMARY_NAME = "stress_summary.txt"
IDENTITY_NAME = "identity.json"
PROGRESS_NAME = "progress.jsonl"

PASS = "PASS"
FAIL = "FAIL"
COLLECTION_FAILED = "COLLECTION_FAILED"

IDENTITY_KEYS = (
    "git_sha",
    "git_ref",
    "git_worktree_clean",
    "product_fingerprint",
    "grader_fingerprint",
    "stress_config_fingerprint",
)


class StressInterrupted(InterruptedError):
    """Signal-safe interruption marker used to preserve partial evidence."""


@dataclass(frozen=True)
class StressConfig:
    cycles: int
    upload: bool
    out_dir: Path
    board_id: str = DEFAULT_BOARD_ID
    port: str = ""
    baud: int = 115200
    timeout_seconds: float = DEFAULT_TIMEOUT_SECONDS

    def contract_parameters(self) -> dict[str, Any]:
        return {
            "cycles": self.cycles,
            "upload": self.upload,
            "board_id": self.board_id,
            "baud": self.baud,
            "timeout_seconds": self.timeout_seconds,
            "handshake_clear_release_trigger": "second_accepted_start",
            "handshake_clear_release_safety_deadline_ms": (
                SECOND_START_RELEASE_SAFETY_DEADLINE_MS
            ),
            "post_ready_observation_ms": int(POST_READY_OBSERVATION_SECONDS * 1000),
            "pre_stop_fence_timeout_ms": int(PRE_STOP_FENCE_TIMEOUT_SECONDS * 1000),
            "first_cycle_wait_ms": int(FIRST_CYCLE_WAIT_SECONDS * 1000),
            "inter_cycle_wait_ms": int(INTER_CYCLE_WAIT_SECONDS * 1000),
            "camera": False,
            "qstart": False,
            "csv": False,
        }


def utc_now() -> str:
    return (
        datetime.now(timezone.utc)
        .replace(microsecond=0)
        .isoformat()
        .replace("+00:00", "Z")
    )


def _canonical_json_bytes(value: Any) -> bytes:
    return json.dumps(
        value,
        sort_keys=True,
        separators=(",", ":"),
        ensure_ascii=True,
    ).encode("utf-8")


def _sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def artifact_entry(path: Path, root: Path) -> dict[str, Any]:
    resolved = path.resolve()
    relative = resolved.relative_to(root.resolve()).as_posix()
    return {
        "path": relative,
        "size_bytes": resolved.stat().st_size,
        "sha256": sha256_file(resolved),
    }


def _atomic_write_json(path: Path, payload: Mapping[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f".{path.name}.tmp-{uuid.uuid4().hex}")
    try:
        with temporary.open("x", encoding="utf-8") as handle:
            json.dump(payload, handle, indent=2, sort_keys=True)
            handle.write("\n")
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary, path)
    finally:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass


def _write_exclusive_json(path: Path, payload: Mapping[str, Any]) -> None:
    with path.open("x", encoding="utf-8") as handle:
        json.dump(payload, handle, indent=2, sort_keys=True)
        handle.write("\n")
        handle.flush()
        os.fsync(handle.fileno())


def _append_progress(path: Path, payload: Mapping[str, Any]) -> None:
    with path.open("a", encoding="utf-8") as handle:
        handle.write(_canonical_json_bytes(payload).decode("ascii") + "\n")
        handle.flush()
        os.fsync(handle.fileno())


def _validate_config(config: StressConfig) -> None:
    if (
        not isinstance(config.cycles, int)
        or isinstance(config.cycles, bool)
        or not 1 <= config.cycles <= MAX_CYCLES
    ):
        raise ValueError(f"cycles must be an integer in 1..{MAX_CYCLES}")
    if (
        not isinstance(config.timeout_seconds, (int, float))
        or isinstance(config.timeout_seconds, bool)
        or not math.isfinite(float(config.timeout_seconds))
        or config.timeout_seconds <= 0
    ):
        raise ValueError("timeout seconds must be a finite positive number")
    if not isinstance(config.baud, int) or isinstance(config.baud, bool) or config.baud <= 0:
        raise ValueError("baud must be a positive integer")
    if (
        not config.board_id
        or config.board_id in {".", ".."}
        or re.fullmatch(r"[A-Za-z0-9._-]+", config.board_id) is None
    ):
        raise ValueError("board id must contain only letters, digits, dot, underscore, or dash")


def _cycle_boundary(ordinal: int, edge: str) -> str:
    return f"reconnect_stress_cycle_{ordinal:04d}_{edge}"


def _write_serial_slice(
    shared_log: Path,
    destination: Path,
    begin_label: str,
    end_label: str,
) -> None:
    lines = shared_log.read_text(encoding="utf-8").splitlines(keepends=True)
    begin_line = f"HOST_BOUNDARY {begin_label}"
    end_line = f"HOST_BOUNDARY {end_label}"
    begins = [index for index, line in enumerate(lines) if line.rstrip("\r\n") == begin_line]
    ends = [index for index, line in enumerate(lines) if line.rstrip("\r\n") == end_line]
    if len(begins) != 1 or len(ends) != 1 or ends[0] <= begins[0]:
        raise RuntimeError("cycle serial boundaries are missing, repeated, or out of order")
    with destination.open("x", encoding="utf-8") as handle:
        handle.writelines(lines[begins[0] : ends[0] + 1])
        handle.flush()
        os.fsync(handle.fileno())


def _start_count(ledger_checks: Mapping[str, Any]) -> int | None:
    raw = ledger_checks.get("start_request_counts")
    if not isinstance(raw, list) or len(raw) != 1 or not isinstance(raw[0], dict):
        return None
    if raw[0].get("epoch") != 1:
        return None
    count = raw[0].get("count")
    if not isinstance(count, int) or isinstance(count, bool) or count < 0:
        return None
    return count


def _start_timing(ledger_checks: Mapping[str, Any]) -> dict[str, Any] | None:
    elapsed = ledger_checks.get("start_elapsed_ms")
    gap = ledger_checks.get("start_gap_ms")
    if (
        not isinstance(elapsed, list)
        or len(elapsed) != 2
        or any(
            not isinstance(value, int) or isinstance(value, bool) or value < 0
            for value in elapsed
        )
        or not isinstance(gap, int)
        or isinstance(gap, bool)
        or gap != elapsed[1] - elapsed[0]
    ):
        return None
    return {"start_elapsed_ms": elapsed, "start_gap_ms": gap}


def _identity_snapshot(identity: Mapping[str, Any]) -> dict[str, Any]:
    return {key: identity.get(key) for key in IDENTITY_KEYS}


def _identity_check(
    expected: Mapping[str, Any],
    observed: Mapping[str, Any],
    stage: str,
) -> dict[str, Any]:
    expected_snapshot = _identity_snapshot(expected)
    observed_snapshot = _identity_snapshot(observed)
    drift = [
        key
        for key in IDENTITY_KEYS
        if expected_snapshot.get(key) != observed_snapshot.get(key)
    ]
    if observed_snapshot.get("git_worktree_clean") is not True:
        drift.append("git_worktree_clean")
    drift = list(dict.fromkeys(drift))
    return {
        "stage": stage,
        "result": FAIL if drift else PASS,
        "drift_fields": drift,
        "observed": observed_snapshot,
    }


def _executable_identity(path: Path) -> dict[str, Any]:
    resolved = path.resolve()
    if not resolved.is_file():
        raise RuntimeError(f"built v1replay executable is missing: {resolved}")
    if not os.access(resolved, os.X_OK):
        raise RuntimeError(f"built v1replay artifact is not executable: {resolved}")
    return {
        "path": str(resolved),
        "size_bytes": resolved.stat().st_size,
        "sha256": sha256_file(resolved),
        "executable": True,
    }


def _executable_check(
    expected: Mapping[str, Any],
    path: Path,
    stage: str,
) -> dict[str, Any]:
    observed = _executable_identity(path)
    drift = [
        field
        for field in ("path", "size_bytes", "sha256", "executable")
        if expected.get(field) != observed.get(field)
    ]
    return {
        "stage": stage,
        "result": FAIL if drift else PASS,
        "drift_fields": drift,
        "observed": observed,
    }


def _classify_cycle(
    ledger_checks: Mapping[str, Any],
    emulator_log_checks: Mapping[str, Any],
    lifecycle_checks: Mapping[str, Any],
    serial_checks: Mapping[str, Any],
    preflight_error: Mapping[str, Any] | None,
    invariant_errors: list[str],
) -> tuple[str, str, list[str]]:
    behavior: list[str] = []
    collection: list[str] = list(invariant_errors)
    timeout_obligations = ledger_checks.get("delivery_obligations")
    timeout_missing_emulator_delivery = False
    if isinstance(timeout_obligations, dict):
        missing_emulator = timeout_obligations.get("missing_emulator_delivery")
        timeout_missing_emulator_delivery = bool(
            isinstance(missing_emulator, list) and missing_emulator
        )
    timeout_collection_failure = bool(
        preflight_error
        and preflight_error.get("failure_kind") == "handshake_timeout"
        and (
            ledger_checks.get("result") == COLLECTION_FAILED
            or timeout_missing_emulator_delivery
        )
    )
    primary_collection_failure = timeout_collection_failure

    if preflight_error:
        message = str(preflight_error.get("message") or "reconnect preflight failed")
        if preflight_error.get("classification") == FAIL and not timeout_collection_failure:
            behavior.append(message)
        else:
            primary_collection_failure = True
            collection.append(message)

    for label, checks in (
        ("ledger", ledger_checks),
        ("emulator log", emulator_log_checks),
        ("lifecycle", lifecycle_checks),
        ("serial lifecycle", serial_checks),
    ):
        result = checks.get("result")
        evidence = [str(item) for item in checks.get("evidence") or []]
        if result == FAIL:
            independently_invalid = (
                label == "ledger" and checks.get("handshake_state") == "invalid"
            )
            if primary_collection_failure and not independently_invalid:
                collection.extend(evidence or [f"{label} grader reported FAIL"])
            else:
                behavior.extend(evidence or [f"{label} grader reported FAIL"])
        elif result != PASS:
            collection.extend(evidence or [f"{label} grader did not report PASS"])

    count = _start_count(ledger_checks)
    if count is None:
        collection.append("stress ledger does not identify one epoch start-request count")
    elif count == 1:
        collection.append(
            "stress stimulus was not exercised: expected exactly two pre-stream starts, observed one"
        )
    elif count > 2:
        behavior.append(
            f"stress handshake exceeded exactly two pre-stream starts: observed {count}"
        )
    elif count != 2:
        collection.append(
            f"stress ledger has an invalid pre-stream start count: observed {count}"
        )
    elif _start_timing(ledger_checks) is None:
        collection.append(
            "stress ledger does not provide the two start elapsed_ms values and exact gap_ms"
        )

    behavior = list(dict.fromkeys(behavior))
    collection = list(dict.fromkeys(collection))
    if behavior:
        return FAIL, "behavior", [*behavior, *collection]
    if collection:
        return COLLECTION_FAILED, "evidence_or_transport", collection
    return PASS, "", []


class ProductionRuntime:
    """Adapters for the real host tools; tests supply a fake with this surface."""

    def __init__(self) -> None:
        bench_dir = ROOT / "scripts" / "bench"
        tools_dir = ROOT / "tools"
        sys.path.insert(0, str(bench_dir))
        sys.path.insert(0, str(tools_dir))
        import bench_identity  # noqa: PLC0415
        import bench_score  # noqa: PLC0415
        import run_window  # noqa: PLC0415

        self.bench_identity = bench_identity
        self.bench_score = bench_score
        self.run_window = run_window

    def now_utc(self) -> str:
        return utc_now()

    def monotonic(self) -> float:
        return time.monotonic()

    def sleep(self, seconds: float) -> None:
        time.sleep(seconds)

    @staticmethod
    def _git(*args: str) -> str:
        process = subprocess.run(
            ["git", *args],
            cwd=ROOT,
            text=True,
            capture_output=True,
            check=False,
        )
        if process.returncode != 0:
            raise RuntimeError(f"git {' '.join(args)} failed")
        return process.stdout.strip()

    def build_identity(self, config: StressConfig) -> dict[str, Any]:
        contract_paths = (
            Path(__file__).resolve(),
            ROOT / "scripts" / "bench" / "run_window.py",
            ROOT / "tools" / "bench_score.py",
        )
        contract_files = {
            path.relative_to(ROOT).as_posix(): {
                "size_bytes": path.stat().st_size,
                "sha256": sha256_file(path),
            }
            for path in contract_paths
        }
        stress_contract = {
            "schema": "reconnect-stress-contract-v1",
            "parameters": config.contract_parameters(),
            "files": contract_files,
        }
        status = self._git("status", "--porcelain")
        return {
            "schema_version": 1,
            "kind": "reconnect_stress_identity",
            "algorithm": "sha256",
            "git_sha": self._git("rev-parse", "HEAD"),
            "git_ref": self._git("rev-parse", "--abbrev-ref", "HEAD"),
            "git_worktree_clean": status == "",
            "product_fingerprint": self.bench_identity.current_product_fingerprint(ROOT),
            "grader_fingerprint": self.bench_identity.current_grader_fingerprint(ROOT),
            "stress_config_fingerprint": _sha256_bytes(
                _canonical_json_bytes(stress_contract)
            ),
            "stress_contract": stress_contract,
        }

    @staticmethod
    def _run_logged(command: list[str], log_path: Path, label: str) -> None:
        with log_path.open("xb") as handle:
            process = subprocess.run(
                command,
                cwd=ROOT,
                stdout=handle,
                stderr=subprocess.STDOUT,
                check=False,
            )
        if process.returncode != 0:
            raise RuntimeError(f"{label} failed with exit {process.returncode}")

    def build_v1replay(self, log_path: Path) -> Path:
        self._run_logged(
            [str(ROOT / "tools" / "v1replay" / "scripts" / "build.sh")],
            log_path,
            "v1replay build",
        )
        return ROOT / "tools" / "v1replay" / ".build" / "v1replay"

    def wait_for_port(self, preferred: str, timeout_seconds: int = 30) -> str:
        return self.run_window.wait_for_port(preferred, timeout_seconds)

    def upload_firmware(self, port: str, log_path: Path) -> None:
        command = [str(ROOT / "build.sh"), "-f", "-u"]
        if port:
            command.extend(["--upload-port", port])
        self._run_logged(command, log_path, "production upload")

    def open_serial(self, port: str, baud: int, log_path: Path) -> Any:
        return self.run_window.BenchSerial(port, baud, log_path)

    def wait_ready(self, serial_session: Any, timeout_seconds: float) -> Any:
        return self.run_window.wait_ready(serial_session, int(math.ceil(timeout_seconds)))

    def establish_readiness(self, serial_session: Any, timeout_seconds: float) -> Any:
        return self.run_window.establish_reconnect_readiness(
            serial_session,
            timeout_seconds,
        )

    def final_fence(self, serial_session: Any, timeout_seconds: float) -> Any:
        return self.run_window.establish_serial_fence(
            serial_session,
            timeout_s=timeout_seconds,
        )

    def make_emulator(self, executable: Path, cycle_dir: Path) -> Any:
        return self.run_window.V1Emulator(
            executable,
            cycle_dir,
            "replay",
            "scenario",
            handshake_only=True,
            # The v1replay constructor keeps its compatibility name, but in
            # handshake-only stress mode this is a release safety deadline:
            # the second accepted START normally releases the clear earlier.
            handshake_notification_hold_ms=(
                SECOND_START_RELEASE_SAFETY_DEADLINE_MS
            ),
        )

    def run_preflight(
        self,
        serial_session: Any,
        emulator: Any,
        timeout_seconds: float,
        *,
        pre_stop_fence_timeout_s: float,
    ) -> dict[str, Any]:
        return self.run_window.run_reconnect_preflight(
            serial_session,
            emulator,
            timeout_seconds,
            post_ready_observation_s=POST_READY_OBSERVATION_SECONDS,
            pre_stop_fence_timeout_s=pre_stop_fence_timeout_s,
        )

    def grade_ledger(self, path: Path) -> dict[str, Any]:
        checks = self.bench_score.score_replay_handshake_ledger(
            path,
            expected_stream_frame=self.bench_score.RECONNECT_PREFLIGHT_CLEAR_FRAME,
        )
        strict = self.bench_score.score_reconnect_epoch("stress", checks)
        if strict.get("result") == COLLECTION_FAILED:
            return strict

        records = [
            json.loads(line)
            for line in path.read_text(encoding="utf-8").splitlines()[1:]
        ]
        start_elapsed_ms = []
        counts = {
            "start_requests": 0,
            "clear_deliveries": 0,
            "version_requests": 0,
            "version_reply_deliveries": 0,
            "all_volume_requests": 0,
            "all_volume_reply_deliveries": 0,
        }
        for event in records:
            if event.get("epoch") != 1 or event.get("event") == "subscribe":
                continue
            decoded, _error = self.bench_score.decode_handshake_frame(event["bytes"])
            if event.get("event") == "request" and decoded == self.bench_score.START_ALERT_REQUEST:
                start_elapsed_ms.append(event["elapsed_ms"])
                counts["start_requests"] += 1
            elif event.get("event") == "request" and decoded == self.bench_score.VERSION_REQUEST:
                counts["version_requests"] += 1
            elif event.get("event") == "request" and decoded == self.bench_score.ALL_VOLUME_REQUEST:
                counts["all_volume_requests"] += 1
            elif (
                event.get("event") == "stream_started"
                and event.get("delivery") == "delivered"
                and tuple(event["bytes"]) == self.bench_score.RECONNECT_PREFLIGHT_CLEAR_FRAME
            ):
                counts["clear_deliveries"] += 1
            elif (
                event.get("event") == "response"
                and event.get("delivery") == "delivered"
                and decoded == self.bench_score.VERSION_RESPONSE
            ):
                counts["version_reply_deliveries"] += 1
            elif (
                event.get("event") == "response"
                and event.get("delivery") == "delivered"
                and decoded == self.bench_score.ALL_VOLUME_RESPONSE
            ):
                counts["all_volume_reply_deliveries"] += 1
        strict["start_elapsed_ms"] = start_elapsed_ms
        strict["start_gap_ms"] = (
            start_elapsed_ms[1] - start_elapsed_ms[0]
            if len(start_elapsed_ms) == 2
            else None
        )
        missing_emulator_delivery = []
        if counts["start_requests"] and not counts["clear_deliveries"]:
            missing_emulator_delivery.append("canonical_clear_row")
        if counts["version_requests"] and not counts["version_reply_deliveries"]:
            missing_emulator_delivery.append("version_reply")
        if counts["all_volume_requests"] and not counts["all_volume_reply_deliveries"]:
            missing_emulator_delivery.append("all_volume_reply")
        missing_firmware_request = []
        if not counts["start_requests"]:
            missing_firmware_request.append("start_request")
        if counts["clear_deliveries"] and not counts["version_requests"]:
            missing_firmware_request.append("version_request")
        if counts["version_reply_deliveries"] and not counts["all_volume_requests"]:
            missing_firmware_request.append("all_volume_request")
        strict["delivery_obligations"] = {
            **counts,
            "missing_emulator_delivery": missing_emulator_delivery,
            "missing_firmware_request": missing_firmware_request,
        }
        return strict

    def grade_lifecycle(
        self,
        raw: Any,
        failure_kind: str = "",
    ) -> dict[str, Any]:
        return self.bench_score.score_reconnect_lifecycle(
            raw,
            failure_kind=failure_kind,
        )

    def grade_emulator_log(self, path: Path) -> dict[str, Any]:
        text, read_error = self.bench_score.read_bounded_reconnect_log(
            path,
            "preflight log",
            self.bench_score.MAX_RECONNECT_PREFLIGHT_LOG_BYTES,
            require_final_newline=False,
        )
        if read_error is not None:
            return read_error
        assert text is not None
        if not text.endswith("\n") and "V1REPLAY_EVENT " in text.rsplit("\n", 1)[-1]:
            return self.bench_score.handshake_collection_failure(
                "reconnect stress emulator log has a truncated machine event"
            )
        events, error = self.bench_score.parse_reconnect_machine_events(text)
        if error:
            return self.bench_score.handshake_collection_failure(
                f"reconnect stress {error}"
            )
        if any(
            event.get("state") in {"replay_started", "complete"}
            for _line_number, event in events
        ):
            return self.bench_score.handshake_collection_failure(
                "reconnect stress emulator entered the scored replay scenario"
            )
        configured = [
            event for _line_number, event in events if event.get("state") == "configured"
        ]
        ready = [
            event for _line_number, event in events if event.get("state") == "handshake_ready"
        ]
        diagnostics = []
        if len(configured) != 1:
            diagnostics.append("emulator log does not contain one configured event")
        if len(ready) != 1:
            diagnostics.append("emulator log does not contain one ready event")
        return {"result": PASS, "evidence": [], "diagnostics": diagnostics}

    def grade_serial_slice(self, path: Path, ordinal: int) -> dict[str, Any]:
        text, read_error = self.bench_score.read_bounded_reconnect_log(
            path,
            "stress cycle serial slice",
            self.bench_score.MAX_BENCH_SERIAL_LOG_BYTES,
        )
        if read_error is not None:
            return read_error
        assert text is not None
        lines = text.splitlines()

        def indexes(value: str) -> list[int]:
            return [index for index, line in enumerate(lines) if line == value]

        def one_index(value: str, label: str) -> tuple[int | None, str]:
            found = indexes(value)
            if len(found) != 1:
                return None, f"stress serial slice requires one {label}"
            return found[0], ""

        def status_fence(start: int, stop: int, label: str) -> str:
            commands = [
                index for index in range(start, stop) if lines[index] == ">>> QSTATUS"
            ]
            responses = [
                index
                for index in range(start, stop)
                if lines[index].startswith(("QRESP ", "QERR "))
            ]
            if len(commands) != 1 or len(responses) != 1:
                return f"stress {label} requires one QSTATUS and one response"
            if responses[0] <= commands[0] or lines[responses[0]].startswith("QERR "):
                return f"stress {label} has an invalid QSTATUS response order"
            try:
                payload = json.loads(lines[responses[0]][len("QRESP ") :])
            except json.JSONDecodeError:
                return f"stress {label} QRESP is malformed"
            if not isinstance(payload, dict) or not (
                payload.get("ok") is True
                and payload.get("state") in {"idle", "done"}
                and payload.get("suite") in {"core", "display"}
                and payload.get("mode") in {"current", "proxy", "obd", "v1"}
            ):
                return f"stress {label} QRESP is not ready"
            return ""

        begin_value = f"HOST_BOUNDARY {_cycle_boundary(ordinal, 'begin')}"
        end_value = f"HOST_BOUNDARY {_cycle_boundary(ordinal, 'end')}"
        expected_boundaries = (
            (begin_value, "outer begin"),
            ("HOST_BOUNDARY reconnect_preflight_start", "preflight start"),
            ("HOST_BOUNDARY reconnect_preflight_fence_begin", "pre-stop fence begin"),
            ("HOST_BOUNDARY reconnect_preflight_fence_complete", "pre-stop fence complete"),
            ("HOST_BOUNDARY reconnect_preflight_process_exited", "process exit"),
            (
                "HOST_BOUNDARY reconnect_post_cleanup_fence_begin",
                "post-cleanup fence begin",
            ),
            (
                "HOST_BOUNDARY reconnect_post_cleanup_fence_complete",
                "post-cleanup fence complete",
            ),
            (end_value, "outer end"),
        )
        positions: list[int] = []
        for value, label in expected_boundaries:
            position, error = one_index(value, label)
            if error:
                return self.bench_score.handshake_collection_failure(error)
            assert position is not None
            positions.append(position)
        if positions != sorted(positions) or len(set(positions)) != len(positions):
            return self.bench_score.handshake_collection_failure(
                "stress serial lifecycle boundaries are out of order"
            )

        (
            outer_begin,
            preflight_start,
            pre_fence_begin,
            pre_fence_complete,
            process_exit,
            post_fence_begin,
            post_fence_complete,
            outer_end,
        ) = positions
        if outer_begin != 0 or outer_end != len(lines) - 1:
            return self.bench_score.handshake_collection_failure(
                "stress serial slice is not bounded by its unique outer markers"
            )
        if any(line.startswith("BOOT bootId=") for line in lines):
            return self.bench_score.handshake_collection_failure(
                "board booted inside reconnect stress cycle"
            )
        forbidden = [
            line
            for line in lines
            if line.startswith((">>> QSTART", ">>> QGETCSV"))
            or "camera" in line.lower()
        ]
        if forbidden:
            return self.bench_score.handshake_collection_failure(
                "stress serial slice contains a forbidden metrics or camera action"
            )

        barrier_commands = [
            index
            for index in range(outer_begin + 1, preflight_start)
            if lines[index].startswith(">>> QBSC08 ")
        ]
        barrier_responses = [
            index
            for index in range(outer_begin + 1, preflight_start)
            if lines[index].startswith("QBSC08 ")
        ]
        if len(barrier_commands) != 1 or len(barrier_responses) != 1:
            return self.bench_score.handshake_collection_failure(
                "stress cycle readiness requires one QBSC08 request and response"
            )
        command_fields = lines[barrier_commands[0]].split()
        if (
            len(command_fields) != 3
            or command_fields[:2] != [">>>", "QBSC08"]
            or len(command_fields[2]) != 32
            or any(character not in "0123456789abcdef" for character in command_fields[2])
        ):
            return self.bench_score.handshake_collection_failure(
                "stress cycle readiness request has an invalid nonce"
            )
        if barrier_responses[0] <= barrier_commands[0]:
            return self.bench_score.handshake_collection_failure(
                "stress cycle readiness response precedes its request"
            )
        try:
            barrier_payload = json.loads(
                lines[barrier_responses[0]][len("QBSC08 ") :]
            )
        except json.JSONDecodeError:
            barrier_payload = None
        if not isinstance(barrier_payload, dict) or not (
            barrier_payload.get("schema") == 1
            and barrier_payload.get("nonce") == command_fields[2]
            and barrier_payload.get("status") in {"ready", "busy"}
        ):
            return self.bench_score.handshake_collection_failure(
                "stress cycle readiness response does not match its nonce"
            )
        readiness_error = status_fence(
            barrier_responses[0] + 1,
            preflight_start,
            "readiness fence",
        )
        if readiness_error:
            return self.bench_score.handshake_collection_failure(readiness_error)
        for start, stop, label in (
            (pre_fence_begin + 1, pre_fence_complete, "pre-stop fence"),
            (post_fence_begin + 1, post_fence_complete, "post-cleanup fence"),
        ):
            error = status_fence(start, stop, label)
            if error:
                return self.bench_score.handshake_collection_failure(error)

        cleanup_indexes = [
            index
            for index, line in enumerate(lines)
            if line.startswith(self.bench_score.V1_DISCONNECT_CLEANUP_PREFIX)
        ]
        valid_cleanups = [
            index
            for index in cleanup_indexes
            if process_exit < index < post_fence_begin
        ]
        if not cleanup_indexes:
            return self.bench_score.handshake_collection_failure(
                "stress serial slice is missing its disconnect cleanup evidence"
            )
        if len(cleanup_indexes) != 1 or len(valid_cleanups) != 1:
            return {
                "result": FAIL,
                "evidence": [
                    "stress cycle requires exactly one cleanup after process exit and before its fence"
                ],
            }
        return {
            "result": PASS,
            "evidence": [],
            "diagnostics": [],
            "readiness_nonce": command_fields[2],
            "cleanup_marker_count": 1,
        }


def _cycle_artifacts(cycle_dir: Path, root: Path) -> list[dict[str, Any]]:
    paths = (
        cycle_dir / LEDGER_NAME,
        cycle_dir / EMULATOR_LOG_NAME,
        cycle_dir / SERIAL_SLICE_NAME,
    )
    return [artifact_entry(path, root) for path in paths if path.is_file()]


def _run_cycle(
    config: StressConfig,
    runtime: Any,
    serial_session: Any,
    executable: Path,
    root: Path,
    ordinal: int,
    boot_anchor: int,
    cleanup_anchor: int,
) -> tuple[dict[str, Any], bool]:
    cycle_dir = root / "cycles" / f"{ordinal:04d}"
    cycle_dir.mkdir(parents=True, exist_ok=False)
    started_utc = runtime.now_utc()
    started_monotonic = runtime.monotonic()
    begin_label = _cycle_boundary(ordinal, "begin")
    end_label = _cycle_boundary(ordinal, "end")
    emulator: Any = None
    preflight_raw: dict[str, Any] = {}
    preflight_error: dict[str, Any] | None = None
    invariant_errors: list[str] = []
    interrupted = False
    end_recorded = False

    serial_session.record_host_boundary(begin_label)
    try:
        runtime.establish_readiness(serial_session, config.timeout_seconds)
        if serial_session.boot_marker_count != boot_anchor:
            raise RuntimeError("board booted between reconnect stress cycles")
        emulator = runtime.make_emulator(executable, cycle_dir)
        preflight_raw = runtime.run_preflight(
            serial_session,
            emulator,
            config.timeout_seconds,
            pre_stop_fence_timeout_s=PRE_STOP_FENCE_TIMEOUT_SECONDS,
        )
    except (KeyboardInterrupt, StressInterrupted) as exc:
        interrupted = True
        preflight_error = {
            "classification": COLLECTION_FAILED,
            "failure_kind": "interrupted",
            "message": str(exc) or "reconnect stress interrupted",
        }
    except Exception as exc:  # noqa: BLE001 - preserve the cycle's terminal evidence
        classification = str(getattr(exc, "classification", COLLECTION_FAILED))
        failure_kind = str(getattr(exc, "failure_kind", "evidence_or_transport"))
        raw_result = getattr(exc, "result", {})
        if isinstance(raw_result, dict):
            preflight_raw = dict(raw_result)
        preflight_error = {
            "classification": FAIL if classification == FAIL else COLLECTION_FAILED,
            "failure_kind": failure_kind,
            "message": str(exc),
        }
    finally:
        if emulator is not None:
            try:
                emulator.stop()
            except Exception as exc:  # noqa: BLE001 - retain cleanup failure
                invariant_errors.append(f"emulator cleanup failed: {exc}")
        try:
            serial_session.record_host_boundary(end_label)
            end_recorded = True
        except Exception as exc:  # noqa: BLE001 - retain serial evidence failure
            invariant_errors.append(f"cycle end boundary failed: {exc}")

    ledger_path = cycle_dir / LEDGER_NAME
    failure_kind = str((preflight_error or {}).get("failure_kind") or "")
    try:
        ledger_checks = runtime.grade_ledger(ledger_path)
    except Exception as exc:  # noqa: BLE001 - grader crash is collection failure
        ledger_checks = {
            "result": COLLECTION_FAILED,
            "evidence": [f"ledger grader failed: {exc}"],
        }
    try:
        emulator_log_checks = runtime.grade_emulator_log(
            cycle_dir / EMULATOR_LOG_NAME
        )
    except Exception as exc:  # noqa: BLE001 - grader crash is collection failure
        emulator_log_checks = {
            "result": COLLECTION_FAILED,
            "evidence": [f"emulator log grader failed: {exc}"],
        }
    try:
        lifecycle_checks = runtime.grade_lifecycle(preflight_raw, failure_kind)
    except Exception as exc:  # noqa: BLE001 - grader crash is collection failure
        lifecycle_checks = {
            "result": COLLECTION_FAILED,
            "evidence": [f"lifecycle grader failed: {exc}"],
        }

    if serial_session.boot_marker_count != boot_anchor:
        invariant_errors.append("board booted during reconnect stress")
    expected_cleanups = cleanup_anchor + ordinal
    if serial_session.disconnect_cleanup_count != expected_cleanups:
        invariant_errors.append(
            "disconnect cleanup count mismatch: "
            f"observed={serial_session.disconnect_cleanup_count - cleanup_anchor} "
            f"expected={ordinal}"
        )

    slice_path = cycle_dir / SERIAL_SLICE_NAME
    if end_recorded:
        try:
            _write_serial_slice(
                root / SERIAL_LOG_NAME,
                slice_path,
                begin_label,
                end_label,
            )
        except Exception as exc:  # noqa: BLE001 - derived evidence must fail closed
            invariant_errors.append(f"cycle serial slice failed: {exc}")
    try:
        serial_checks = runtime.grade_serial_slice(slice_path, ordinal)
    except Exception as exc:  # noqa: BLE001 - offline grader fails closed
        serial_checks = {
            "result": COLLECTION_FAILED,
            "evidence": [f"serial lifecycle grader failed: {exc}"],
        }

    result, classification, evidence = _classify_cycle(
        ledger_checks,
        emulator_log_checks,
        lifecycle_checks,
        serial_checks,
        preflight_error,
        invariant_errors,
    )
    cycle_failure_kind = failure_kind or classification
    payload = {
        "schema_version": 1,
        "kind": "reconnect_stress_cycle",
        "cycle": ordinal,
        "result": result,
        "classification": classification,
        "failure_kind": cycle_failure_kind,
        "started_utc": started_utc,
        "finished_utc": runtime.now_utc(),
        "elapsed_seconds": round(max(0.0, runtime.monotonic() - started_monotonic), 3),
        "ledger_checks": ledger_checks,
        "emulator_log_checks": emulator_log_checks,
        "lifecycle_checks": lifecycle_checks,
        "serial_checks": serial_checks,
        "start_timing": _start_timing(ledger_checks),
        "preflight_terminal": preflight_raw,
        "evidence": evidence,
        "artifacts": _cycle_artifacts(cycle_dir, root),
    }
    result_path = cycle_dir / "cycle_result.json"
    _write_exclusive_json(result_path, payload)
    payload["cycle_result"] = artifact_entry(result_path, root)
    return payload, interrupted


def _terminal_artifacts(root: Path) -> list[dict[str, Any]]:
    result_path = root / RESULT_NAME
    artifacts = []
    for path in sorted(root.rglob("*")):
        if not path.is_file() or path == result_path or ".tmp-" in path.name:
            continue
        artifacts.append(artifact_entry(path, root))
    return artifacts


def _render_summary(payload: Mapping[str, Any]) -> str:
    lines = [
        f"reconnect stress: {payload.get('result')}",
        f"cycles: {payload.get('passed_cycles')}/{payload.get('requested_cycles')} PASS",
    ]
    qualification = payload.get("qualification")
    if isinstance(qualification, dict):
        lines.append(
            "qualification: "
            + ("QUALIFYING" if qualification.get("qualifying") is True else "NON-QUALIFYING")
            + f" ({qualification.get('reason') or 'unspecified'})"
        )
    first_failure = payload.get("first_failure_cycle")
    if first_failure:
        lines.append(
            f"first failure: cycle {first_failure} ({payload.get('failure_kind') or 'unknown'})"
        )
    if payload.get("error"):
        lines.append(f"error: {payload['error']}")
    lines.append(f"artifacts: {payload.get('artifact_dir', '')}")
    return "\n".join(lines) + "\n"


def _qualification(
    upload_requested: bool,
    upload_completed: bool,
    result: str,
) -> dict[str, Any]:
    if not upload_requested:
        return {
            "qualifying": False,
            "flash_provenance": "unverified_existing_board",
            "reason": "--no-upload cannot bind the board's installed firmware to the recorded source identity.",
        }
    if not upload_completed:
        return {
            "qualifying": False,
            "flash_provenance": "upload_not_completed",
            "reason": "The requested production upload and post-upload port reacquisition did not complete.",
        }
    if result != PASS:
        return {
            "qualifying": False,
            "flash_provenance": "built_and_uploaded_by_runner",
            "reason": "The runner flashed the board, but the stress gate did not pass.",
        }
    return {
        "qualifying": True,
        "flash_provenance": "built_and_uploaded_by_runner",
        "reason": "The runner built and flashed the recorded source before all cycles passed.",
    }


def _board_firmware_identity(
    upload_requested: bool,
    upload_completed: bool,
    identity: Mapping[str, Any],
) -> dict[str, Any]:
    if not upload_requested:
        return {
            "result": "N/A",
            "reason": "The installed board firmware was not uploaded or identified by this run; host source fingerprints must not be attributed to it.",
        }
    if not upload_completed:
        return {
            "result": "N/A",
            "reason": "The requested upload did not complete through post-upload port reacquisition, so board firmware identity is unverified.",
        }
    return {
        "result": "VERIFIED",
        "method": "built_and_uploaded_by_runner",
        "git_sha": identity.get("git_sha", ""),
        "product_fingerprint": identity.get("product_fingerprint", ""),
        "reason": "The runner built and uploaded production firmware from the recorded clean source identity.",
    }


def _evidence_tiers(
    result: str,
    passed_cycles: int,
    attempted_cycles: int,
) -> dict[str, Any]:
    hil_result = result if attempted_cycles else "N/A"
    hil_reason = (
        "A connected board supplied the handshake and disconnect behavior under test."
        if attempted_cycles
        else "No reconnect stress cycle reached a complete board evidence record."
    )
    return {
        "vendor_spec": {
            "result": "N/A",
            "reason": "The stress contract exercises V1Simple and its managed emulator, not a vendor claim.",
        },
        "hil_board": {
            "result": hil_result,
            "cycles_passed": passed_cycles,
            "reason": hil_reason,
        },
        "camera": {
            "result": "N/A",
            "reason": "Reconnect stress has no display-semantic or camera-validation step.",
        },
        "physical_v1": {
            "result": "N/A",
            "reason": "The peer is the managed v1replay peripheral, not a physical V1.",
        },
    }


def run_stress(config: StressConfig, runtime: Any) -> int:
    _validate_config(config)
    root = config.out_dir.resolve()
    root.mkdir(parents=True, exist_ok=False)
    result_path = root / RESULT_NAME
    progress_path = root / PROGRESS_NAME
    started_utc = runtime.now_utc()
    started_monotonic = runtime.monotonic()
    base_result = {
        "schema_version": 1,
        "kind": "reconnect_stress_result",
        "state": "running",
        "result": "RUNNING",
        "requested_cycles": config.cycles,
        "attempted_cycles": 0,
        "passed_cycles": 0,
        "started_utc": started_utc,
        "board_id": config.board_id,
        "upload": config.upload,
        "stress_parameters": config.contract_parameters(),
        "artifact_dir": str(root),
    }
    _atomic_write_json(result_path, base_result)

    serial_session: Any = None
    cycle_summaries: list[dict[str, Any]] = []
    identity: dict[str, Any] = {}
    identity_checks: list[dict[str, Any]] = []
    executable: Path | None = None
    executable_identity: dict[str, Any] = {}
    executable_checks: list[dict[str, Any]] = []
    upload_completed = False
    final_result = COLLECTION_FAILED
    exit_code = 3
    first_failure_cycle: int | None = None
    failure_kind = ""
    terminal_error = ""
    interrupted = False
    boot_anchor = 0
    cleanup_anchor = 0

    try:
        identity = runtime.build_identity(config)
        _write_exclusive_json(root / IDENTITY_NAME, identity)
        identity_checks.append(_identity_check(identity, identity, "initial"))
        if identity.get("git_worktree_clean") is not True:
            raise RuntimeError("reconnect stress requires a clean worktree")

        executable = Path(runtime.build_v1replay(root / "v1replay_build.log"))
        executable_identity = _executable_identity(executable)
        identity["v1replay_executable"] = executable_identity
        _atomic_write_json(root / IDENTITY_NAME, identity)
        port = runtime.wait_for_port(config.port, 30)
        if config.upload:
            runtime.upload_firmware(port, root / "firmware_upload.log")
            port = runtime.wait_for_port(port, 30)
            upload_completed = True

        post_build_identity = runtime.build_identity(config)
        post_build_check = _identity_check(
            identity,
            post_build_identity,
            "post_build_upload",
        )
        identity_checks.append(post_build_check)
        if post_build_check["result"] != PASS:
            raise RuntimeError(
                "identity drift before reconnect stress hardware cycles: "
                + ", ".join(post_build_check["drift_fields"])
            )
        post_build_executable_check = _executable_check(
            executable_identity,
            executable,
            "post_build_upload",
        )
        executable_checks.append(post_build_executable_check)
        if post_build_executable_check["result"] != PASS:
            raise RuntimeError("v1replay executable changed after build/upload")

        serial_session = runtime.open_serial(port, config.baud, root / SERIAL_LOG_NAME)
        runtime.wait_ready(serial_session, config.timeout_seconds)
        # One initial barrier anchors the complete stress session.  Every cycle
        # then has its own nonce barrier inside its serial slice as well.
        runtime.establish_readiness(serial_session, config.timeout_seconds)
        boot_anchor = serial_session.boot_marker_count
        cleanup_anchor = serial_session.disconnect_cleanup_count

        for ordinal in range(1, config.cycles + 1):
            # The first owner must cross DATA_STALE_MS after a fresh boot;
            # later owners retain the one-second retry/scanner quiet period.
            runtime.sleep(
                FIRST_CYCLE_WAIT_SECONDS
                if ordinal == 1
                else INTER_CYCLE_WAIT_SECONDS
            )
            pre_cycle_executable_check = _executable_check(
                executable_identity,
                executable,
                f"before_cycle_{ordinal:04d}",
            )
            executable_checks.append(pre_cycle_executable_check)
            if pre_cycle_executable_check["result"] != PASS:
                raise RuntimeError(
                    f"v1replay executable drift before cycle {ordinal}"
                )
            cycle_payload, cycle_interrupted = _run_cycle(
                config,
                runtime,
                serial_session,
                executable,
                root,
                ordinal,
                boot_anchor,
                cleanup_anchor,
            )
            cycle_result_path = root / cycle_payload["cycle_result"]["path"]
            progress_entry = {
                "cycle": ordinal,
                "result": cycle_payload["result"],
                "classification": cycle_payload["classification"],
                "failure_kind": cycle_payload["failure_kind"],
                "start_request_count": _start_count(cycle_payload["ledger_checks"]),
                "start_timing": cycle_payload["start_timing"],
                "cycle_result": artifact_entry(cycle_result_path, root),
            }
            _append_progress(progress_path, progress_entry)
            cycle_summaries.append(progress_entry)
            _atomic_write_json(
                result_path,
                {
                    **base_result,
                    "attempted_cycles": len(cycle_summaries),
                    "passed_cycles": sum(
                        item.get("result") == PASS for item in cycle_summaries
                    ),
                    "last_cycle": ordinal,
                    "identity": _identity_snapshot(identity),
                    "v1replay_executable": executable_identity,
                    "cycles": cycle_summaries,
                },
            )
            print(
                f"[reconnect-stress] cycle {ordinal:04d}/{config.cycles:04d} "
                f"{cycle_payload['result']}",
                flush=True,
            )

            if cycle_interrupted:
                interrupted = True
                first_failure_cycle = ordinal
                failure_kind = "interrupted"
                terminal_error = "reconnect stress interrupted"
                break
            if cycle_payload["result"] != PASS:
                first_failure_cycle = ordinal
                failure_kind = str(
                    cycle_payload.get("failure_kind")
                    or cycle_payload.get("classification")
                    or "unknown"
                )
                terminal_error = "; ".join(str(item) for item in cycle_payload["evidence"])
                final_result = str(cycle_payload["result"])
                exit_code = 2 if final_result == FAIL else 3
                break
        else:
            runtime.final_fence(serial_session, config.timeout_seconds)
            if serial_session.boot_marker_count != boot_anchor:
                raise RuntimeError("board booted before reconnect stress completion")
            cleanup_delta = serial_session.disconnect_cleanup_count - cleanup_anchor
            if cleanup_delta != config.cycles:
                raise RuntimeError(
                    f"final disconnect cleanup count={cleanup_delta} expected={config.cycles}"
                )
            final_result = PASS
            exit_code = 0
    except (KeyboardInterrupt, StressInterrupted) as exc:
        interrupted = True
        terminal_error = str(exc) or "reconnect stress interrupted"
        failure_kind = "interrupted"
        exit_code = 130
    except Exception as exc:  # noqa: BLE001 - publish setup/collection terminal
        terminal_error = str(exc)
        failure_kind = failure_kind or "evidence_or_transport"
        final_result = COLLECTION_FAILED
        exit_code = 3
    finally:
        if serial_session is not None:
            try:
                serial_session.close()
            except Exception as exc:  # noqa: BLE001 - report close failure
                if exit_code == 0:
                    final_result = COLLECTION_FAILED
                    exit_code = 3
                    failure_kind = "serial_close"
                    terminal_error = f"serial close failed: {exc}"

    if identity:
        try:
            terminal_identity = runtime.build_identity(config)
            terminal_identity_check = _identity_check(
                identity,
                terminal_identity,
                "terminal",
            )
            identity_checks.append(terminal_identity_check)
            if terminal_identity_check["result"] != PASS:
                raise RuntimeError(
                    "terminal identity drift: "
                    + ", ".join(terminal_identity_check["drift_fields"])
                )
        except (KeyboardInterrupt, StressInterrupted) as exc:
            interrupted = True
            terminal_error = str(exc) or "reconnect stress interrupted"
            failure_kind = "interrupted"
        except Exception as exc:  # noqa: BLE001 - terminal evidence fails closed
            terminal_error = "; ".join(
                item for item in (terminal_error, str(exc)) if item
            )
            failure_kind = "identity_drift"
            final_result = COLLECTION_FAILED
            exit_code = 3

    if executable is not None and executable_identity:
        try:
            terminal_executable_check = _executable_check(
                executable_identity,
                executable,
                "terminal",
            )
            executable_checks.append(terminal_executable_check)
            if terminal_executable_check["result"] != PASS:
                raise RuntimeError(
                    "terminal v1replay executable drift: "
                    + ", ".join(terminal_executable_check["drift_fields"])
                )
        except (KeyboardInterrupt, StressInterrupted) as exc:
            interrupted = True
            terminal_error = str(exc) or "reconnect stress interrupted"
            failure_kind = "interrupted"
        except Exception as exc:  # noqa: BLE001 - terminal evidence fails closed
            terminal_error = "; ".join(
                item for item in (terminal_error, str(exc)) if item
            )
            failure_kind = "executable_drift"
            final_result = COLLECTION_FAILED
            exit_code = 3

    if interrupted:
        final_result = COLLECTION_FAILED
        exit_code = 130
        failure_kind = "interrupted"

    passed_cycles = sum(item.get("result") == PASS for item in cycle_summaries)
    attempted_cycles = len(cycle_summaries)
    terminal = {
        **base_result,
        "state": "terminal",
        "result": final_result,
        "exit_code": exit_code,
        "attempted_cycles": attempted_cycles,
        "passed_cycles": passed_cycles,
        "first_failure_cycle": first_failure_cycle,
        "failure_kind": failure_kind,
        "error": terminal_error,
        "finished_utc": runtime.now_utc(),
        "elapsed_seconds": round(max(0.0, runtime.monotonic() - started_monotonic), 3),
        "identity": {
            key: identity.get(key, "") for key in IDENTITY_KEYS
        },
        "identity_checks": identity_checks,
        "board_firmware_identity": _board_firmware_identity(
            config.upload,
            upload_completed,
            identity,
        ),
        "v1replay_executable": executable_identity,
        "v1replay_executable_checks": executable_checks,
        "cycles": cycle_summaries,
        "qualification": _qualification(
            config.upload,
            upload_completed,
            final_result,
        ),
        "evidence_tiers": _evidence_tiers(
            final_result,
            passed_cycles,
            attempted_cycles,
        ),
    }
    # The summary is owned by the terminal manifest, so publish it before the
    # final recursive artifact hash sweep.
    summary_path = root / SUMMARY_NAME
    summary_path.write_text(_render_summary(terminal), encoding="utf-8")
    terminal["artifacts"] = _terminal_artifacts(root)
    _atomic_write_json(result_path, terminal)
    print(_render_summary(terminal), end="", flush=True)
    return exit_code


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cycles", required=True, type=int)
    flash = parser.add_mutually_exclusive_group(required=True)
    flash.add_argument(
        "--upload",
        action="store_true",
        help="Build and flash production firmware once (required for a qualifying pass)",
    )
    flash.add_argument(
        "--no-upload",
        action="store_true",
        help="Use installed firmware (non-qualifying because flash provenance is unverified)",
    )
    parser.add_argument("--board-id", default=DEFAULT_BOARD_ID)
    parser.add_argument("--port", default="")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--timeout-seconds", type=float, default=DEFAULT_TIMEOUT_SECONDS)
    parser.add_argument("--artifact-root", default=str(DEFAULT_ARTIFACT_ROOT))
    parser.add_argument("--out-dir", default="", help="Explicit new output directory (must not exist)")
    return parser.parse_args(argv)


def _git_short_sha() -> str:
    process = subprocess.run(
        ["git", "rev-parse", "--short", "HEAD"],
        cwd=ROOT,
        text=True,
        capture_output=True,
        check=False,
    )
    return process.stdout.strip() if process.returncode == 0 else "unknown"


def _default_out_dir(artifact_root: Path, board_id: str) -> Path:
    timestamp = datetime.now(timezone.utc).strftime("%Y%m%d_%H%M%S")
    base = artifact_root.resolve() / board_id / "runs" / f"{timestamp}_{_git_short_sha()}"
    candidate = base
    suffix = 2
    while candidate.exists():
        candidate = base.with_name(f"{base.name}_{suffix}")
        suffix += 1
    return candidate


def _signal_handler(signum: int, _frame: Any) -> None:
    raise StressInterrupted(f"received signal {signum}")


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    out_dir = (
        Path(args.out_dir).resolve()
        if args.out_dir
        else _default_out_dir(Path(args.artifact_root), args.board_id)
    )
    config = StressConfig(
        cycles=args.cycles,
        upload=bool(args.upload),
        out_dir=out_dir,
        board_id=args.board_id,
        port=args.port,
        baud=args.baud,
        timeout_seconds=args.timeout_seconds,
    )
    for signum in (signal.SIGTERM, signal.SIGHUP):
        signal.signal(signum, _signal_handler)
    try:
        return run_stress(config, ProductionRuntime())
    except (FileExistsError, ValueError) as exc:
        print(f"reconnect stress setup failed: {exc}", file=sys.stderr)
        return 3


if __name__ == "__main__":
    raise SystemExit(main())
