#!/usr/bin/env python3
"""Collect one SD-backed bench window over USB serial.

This is intentionally only a collector/importer. It starts one firmware bench
window (core, display, or replay), downloads the SD perf CSV, and also downloads
the encounter CSV for replay. It imports the perf data with the shared CSV
importer and writes a small window_result.json. It does not decide release
verdicts, apply OBD/proxy coverage, or promote baselines; optional baseline
manifests are passed through only for importer comparison output.
"""

from __future__ import annotations

import argparse
import binascii
import glob
import json
import os
import signal
import shutil
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Callable

from camera_capture import CameraCapture
from camera_grade import grade_camera

try:  # pyserial is needed only for live collection, not --from-csv imports.
    import serial  # type: ignore
except ImportError:  # pragma: no cover - exercised only on hosts without pyserial
    serial = None  # type: ignore

ROOT = Path(__file__).resolve().parents[2]
IMPORT_PERF_CSV = ROOT / "tools" / "import_perf_csv.py"
BUILD_SH = ROOT / "build.sh"
RUN_PROGRESS_INTERVAL_S = 15


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--suite", choices=["core", "display", "replay"], required=True)
    parser.add_argument("--duration-seconds", type=int, default=300)
    parser.add_argument("--out-dir", required=True)
    parser.add_argument("--port", default=os.environ.get("DEVICE_PORT", ""))
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--board-id", default=os.environ.get("BENCH_BOARD_ID", "release"))
    parser.add_argument("--git-sha", default="")
    parser.add_argument("--git-ref", default="")
    parser.add_argument("--git-worktree-clean", choices=["0", "1"], default="0")
    parser.add_argument("--profile", default="drive_wifi_off")
    parser.add_argument("--segment", default="last")
    parser.add_argument(
        "--compare-to",
        action="append",
        default=[],
        help="Optional baseline manifest.json passed through to the CSV importer",
    )
    parser.add_argument("--lane", default="bench")
    parser.add_argument("--upload", action="store_true", help="Build/upload production firmware+filesystem first")
    parser.add_argument("--skip-web", action="store_true", help="Pass --skip-web to build.sh when uploading")
    parser.add_argument(
        "--post-upload-settle-seconds",
        type=int,
        default=90,
        help="Unscored SD settle interval after upload and before the scored QSTART window",
    )
    parser.add_argument("--from-csv", default="", help="Import an existing perf CSV instead of collecting live")
    parser.add_argument(
        "--replay-executable",
        default="",
        help="v1replay executable used as the V1 emulator after QSTART acknowledgement",
    )
    blink_group = parser.add_mutually_exclusive_group()
    blink_group.add_argument(
        "--blink-profile",
        choices=["scenario", "steady", "stress"],
        default=None,
        help="Priority-arrow blink profile for replay (default: scenario)",
    )
    blink_group.add_argument(
        "--blink-arrow",
        action="store_true",
        help="Legacy alias for --blink-profile stress",
    )
    parser.add_argument("--camera", action="store_true", help="Capture calibrated camera evidence for this window")
    parser.add_argument("--ready-timeout-seconds", type=int, default=45)
    parser.add_argument("--completion-grace-seconds", type=int, default=45)
    parser.add_argument("--export-idle-timeout-seconds", type=int, default=30)
    parser.add_argument("--export-retries", type=int, default=2)
    parser.add_argument("--export-recovery-idle-timeout-seconds", type=int, default=120)
    return parser.parse_args()


def utc_now() -> str:
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")


def write_window_result(out_dir: Path, payload: dict[str, Any]) -> None:
    payload.setdefault("schema_version", 1)
    payload.setdefault("timestamp_utc", utc_now())
    (out_dir / "window_result.json").write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")


def install_signal_handlers() -> None:
    def interrupt(signum: int, _frame: Any) -> None:
        raise InterruptedError(f"received signal {signum}")

    for signum in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
        signal.signal(signum, interrupt)


def detect_port() -> str:
    patterns = [
        "/dev/cu.usbmodem*",
        "/dev/ttyACM*",
        "/dev/ttyUSB*",
        "/dev/cu.usbserial*",
        "/dev/tty.usbserial*",
        "/dev/cu.SLAB_USBtoUART*",
        "/dev/tty.SLAB_USBtoUART*",
    ]
    candidates: list[str] = []
    for pattern in patterns:
        candidates.extend(glob.glob(pattern))
    candidates = sorted(dict.fromkeys(candidates))
    return candidates[0] if candidates else ""


def wait_for_port(preferred: str, timeout_s: int = 30) -> str:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        if preferred and Path(preferred).exists():
            return preferred
        detected = detect_port()
        if detected:
            return detected
        time.sleep(1)
    raise RuntimeError("No USB serial device detected")


def run_upload(port: str, skip_web: bool) -> None:
    cmd = [str(BUILD_SH), "-f", "-u"]
    if skip_web:
        cmd.append("--skip-web")
    if port:
        cmd.extend(["--upload-port", port])
    subprocess.run(cmd, cwd=ROOT, check=True)


def wait_for_post_upload_settle(
    seconds: int,
    *,
    sleep: Callable[[float], None] = time.sleep,
) -> None:
    """Wait in short, signal-interruptible steps before scored collection."""
    if seconds <= 0:
        return
    print(
        f"[bench] allowing {seconds}s for post-upload SD activity to settle before scored collection",
        flush=True,
    )
    remaining = float(seconds)
    while remaining > 0:
        interval = min(1.0, remaining)
        sleep(interval)
        remaining -= interval
    print("[bench] post-upload SD settle complete", flush=True)


class BenchSerial:
    def __init__(self, port: str, baud: int, log_path: Path):
        if serial is None:
            raise RuntimeError("pyserial is required for live bench collection")
        self.port = port
        self.baud = baud
        self.log_path = log_path
        self.log_path.parent.mkdir(parents=True, exist_ok=True)
        self.log = self.log_path.open("a", encoding="utf-8")
        self.ser = serial.Serial()  # type: ignore[union-attr]
        self.ser.port = port
        self.ser.baudrate = baud
        self.ser.timeout = 0.25
        self.ser.write_timeout = 2
        self.ser.dtr = False
        self.ser.rts = False
        self.ser.open()
        self.ser.reset_input_buffer()

    def close(self) -> None:
        try:
            if self.ser.is_open:
                self.ser.close()
        finally:
            self.log.close()

    def write_command(self, command: str) -> None:
        line = command.rstrip("\r\n") + "\n"
        self.log.write(f">>> {line}")
        self.log.flush()
        self.ser.write(line.encode("utf-8"))
        self.ser.flush()

    def read_line(self, timeout_s: float) -> str:
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            raw = self.ser.readline()
            if not raw:
                continue
            text = raw.decode("utf-8", errors="replace").rstrip("\r\n")
            self.log.write(text + "\n")
            self.log.flush()
            return text
        raise TimeoutError("serial read timed out")

    def read_protocol_line(self, prefixes: tuple[str, ...], timeout_s: float) -> str:
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            remaining = max(0.1, deadline - time.monotonic())
            try:
                line = self.read_line(min(0.5, remaining))
            except TimeoutError:
                continue
            if line.startswith(prefixes):
                return line
        raise TimeoutError(f"timed out waiting for {prefixes}")


def parse_json_line(line: str, prefix: str) -> dict[str, Any]:
    if not line.startswith(prefix):
        raise RuntimeError(f"expected {prefix!r}, got: {line}")
    return json.loads(line[len(prefix):])


def wait_ready(q: BenchSerial, timeout_s: int) -> dict[str, Any]:
    deadline = time.monotonic() + timeout_s
    last_error = ""
    while time.monotonic() < deadline:
        q.write_command("QSTATUS")
        try:
            line = q.read_protocol_line(("QRESP ", "QERR "), 2)
        except TimeoutError as exc:
            last_error = str(exc)
            time.sleep(1)
            continue
        if line.startswith("QRESP "):
            return parse_json_line(line, "QRESP ")
        last_error = str(parse_json_line(line, "QERR "))
        time.sleep(1)
    raise RuntimeError(f"bench serial protocol did not become ready: {last_error}")


def start_and_wait(
    q: BenchSerial,
    suite: str,
    duration_s: int,
    grace_s: int,
    after_started: Callable[[], None] | None = None,
    health_check: Callable[[], str] | None = None,
) -> dict[str, Any]:
    start_deadline = time.monotonic() + 15
    last_start_error: dict[str, Any] | None = None
    start_payload: dict[str, Any] | None = None
    firmware_suite = "core" if suite == "replay" else suite
    command = f"QSTART {firmware_suite} {duration_s}"
    while time.monotonic() < start_deadline:
        q.write_command(command)
        attempt_deadline = min(start_deadline, time.monotonic() + 5)
        retry_start = False
        while time.monotonic() < attempt_deadline:
            remaining = max(0.1, attempt_deadline - time.monotonic())
            try:
                line = q.read_protocol_line(("QRESP ", "QERR "), remaining)
            except TimeoutError as exc:
                last_start_error = {"timeout": str(exc)}
                break
            if line.startswith("QRESP "):
                payload = parse_json_line(line, "QRESP ")
                if (
                    payload.get("ok")
                    and payload.get("state") == "running"
                    and payload.get("suite") == firmware_suite
                ):
                    start_payload = payload
                    break
                last_start_error = {"stale_response": payload}
                continue
            last_start_error = parse_json_line(line, "QERR ")
            retry_reason = str(last_start_error.get("error") or last_start_error.get("message") or "")
            if retry_reason == "perf_sd_busy_retry":
                retry_start = True
                break
            raise RuntimeError(f"QSTART failed: {last_start_error}")
        if start_payload is not None:
            break
        if retry_start:
            time.sleep(0.25)
            continue
    if start_payload is None:
        raise RuntimeError(f"QSTART did not produce a running acknowledgement: {last_start_error}")

    print(
        f"[bench] started suite={suite} duration={duration_s}s csv={start_payload.get('csvPath') or 'unknown'}; "
        "metrics are recording to SD",
        flush=True,
    )
    if after_started is not None:
        after_started()

    deadline = time.monotonic() + duration_s + grace_s
    run_started = time.monotonic()
    next_progress = run_started + RUN_PROGRESS_INTERVAL_S
    last_event: dict[str, Any] = start_payload
    while time.monotonic() < deadline:
        if health_check is not None:
            problem = health_check()
            if problem:
                try:
                    q.write_command("QABORT")
                except Exception:  # noqa: BLE001 - retain the original companion failure
                    pass
                raise RuntimeError(problem)
        try:
            line = q.read_protocol_line(("QEVENT ", "QERR "), 1)
        except TimeoutError:
            now = time.monotonic()
            if now >= next_progress:
                elapsed_s = min(duration_s, int(now - run_started))
                print(f"[bench] running suite={suite}: {elapsed_s}/{duration_s}s elapsed", flush=True)
                next_progress = now + RUN_PROGRESS_INTERVAL_S
            continue
        prefix = "QEVENT " if line.startswith("QEVENT ") else "QERR "
        payload = parse_json_line(line, prefix)
        last_event = payload
        if payload.get("state") in {"done", "error"}:
            if not payload.get("ok"):
                raise RuntimeError(f"bench window failed: {payload}")
            print(f"[bench] firmware completed suite={suite}: {payload}", flush=True)
            return payload
    raise RuntimeError(f"bench window timed out waiting for completion; last={last_event}")


def download_csv(q: BenchSerial, out_dir: Path, idle_timeout_s: int, sd_path: str = "") -> Path:
    command = "QGETCSV"
    if sd_path:
        command += f" {sd_path}"
    q.write_command(command)
    line = q.read_protocol_line(("QFILE ", "QERR "), 10)
    if line.startswith("QERR "):
        raise RuntimeError(f"QGETCSV failed: {line}")
    header = parse_json_line(line, "QFILE ")
    path = str(header.get("path") or "perf.csv")
    expected_size = int(header.get("size") or 0)
    basename = Path(path).name or "perf.csv"
    csv_path = out_dir / basename
    print(f"[bench] exporting SD CSV path={path} size={expected_size} bytes", flush=True)

    payload = bytearray()
    expected_seq = 0
    while True:
        line = q.read_protocol_line(("QCHUNK ", "QEND ", "QERR "), idle_timeout_s)
        if line.startswith("QERR "):
            raise RuntimeError(f"CSV export failed: {line}")
        if line.startswith("QEND "):
            end = parse_json_line(line, "QEND ")
            if int(end.get("bytes") or 0) != len(payload):
                raise RuntimeError(f"CSV byte count mismatch: firmware={end.get('bytes')} host={len(payload)}")
            reported_crc = str(end.get("crc32") or "").upper()
            host_crc = f"{binascii.crc32(payload) & 0xFFFFFFFF:08X}"
            if reported_crc and reported_crc != host_crc:
                raise RuntimeError(f"CSV CRC mismatch: firmware={reported_crc} host={host_crc}")
            break
        _prefix, seq_text, hex_text = line.split(" ", 2)
        seq = int(seq_text)
        if seq != expected_seq:
            raise RuntimeError(f"CSV chunk sequence mismatch: expected {expected_seq}, got {seq}")
        payload.extend(bytes.fromhex(hex_text.strip()))
        expected_seq += 1

    if expected_size and expected_size != len(payload):
        raise RuntimeError(f"CSV size mismatch: header={expected_size} downloaded={len(payload)}")
    csv_path.write_bytes(payload)
    print(f"[bench] downloaded CSV to {csv_path} ({len(payload)} bytes)", flush=True)
    return csv_path


def encounter_csv_sd_path(perf_csv_sd_path: str) -> str:
    prefix = "/perf/perf_boot_"
    if not perf_csv_sd_path.startswith(prefix) or not perf_csv_sd_path.endswith(".csv"):
        return ""
    boot_suffix = perf_csv_sd_path[len(prefix) :]
    boot_identity = boot_suffix[: -len(".csv")]
    if not boot_identity or "/" in boot_identity or ".." in boot_identity:
        return ""
    return f"/encounters/encounters_{boot_suffix}"


def run_import(args: argparse.Namespace, csv_path: Path, out_dir: Path) -> subprocess.CompletedProcess[str]:
    stress_class = "core" if args.suite in {"core", "replay"} else "display_preview"
    cmd = [
        sys.executable,
        str(IMPORT_PERF_CSV),
        "--input",
        str(csv_path),
        "--out-dir",
        str(out_dir),
        "--board-id",
        args.board_id,
        "--git-sha",
        args.git_sha,
        "--git-ref",
        args.git_ref,
        "--profile",
        args.profile,
        "--segment",
        args.segment,
        "--stress-class",
        stress_class,
        "--lane",
        f"{args.lane}-{args.suite}",
    ]
    for baseline in args.compare_to:
        if baseline:
            cmd.extend(["--compare-to", baseline])
    proc = subprocess.run(cmd, cwd=ROOT, text=True, capture_output=True, check=False)
    (out_dir / "import_stdout.log").write_text(proc.stdout, encoding="utf-8")
    (out_dir / "import_stderr.log").write_text(proc.stderr, encoding="utf-8")
    return proc


class V1Emulator:
    """Own the LightBlue-compatible V1 emulator for one complete window."""

    COMPLETE_MARKER = b'V1REPLAY_EVENT {"state":"complete"}'

    def __init__(
        self,
        executable: Path,
        out_dir: Path,
        suite: str,
        blink_profile: str | None = None,
    ):
        self.executable = executable
        self.suite = suite
        self.mode = "bench" if suite == "replay" else "idle"
        self.blink_profile = blink_profile or ("scenario" if suite == "replay" else "steady")
        self.log_path = out_dir / "v1replay.log"
        self.process: subprocess.Popen[bytes] | None = None
        self.log_handle: Any = None
        self.started = False
        self.started_monotonic: float | None = None
        self.completed = False
        self.managed_stop = False
        self.returncode: int | None = None

    def start(self) -> None:
        if not self.executable.is_file() or not os.access(self.executable, os.X_OK):
            raise RuntimeError("v1replay executable is missing or not executable")
        self.log_path.parent.mkdir(parents=True, exist_ok=True)
        self.log_handle = self.log_path.open("wb")
        command = [str(self.executable), self.mode]
        if self.mode == "bench":
            command.extend(["--machine-events", "--blink-profile", self.blink_profile])
        self.process = subprocess.Popen(
            command,
            cwd=self.executable.parent.parent,
            stdout=self.log_handle,
            stderr=subprocess.STDOUT,
            start_new_session=True,
        )
        self.started_monotonic = time.monotonic()
        self.started = True
        print(f"[bench] launched V1 emulator mode={self.mode}; log: {self.log_path}", flush=True)

    def health_problem(self) -> str:
        if self.process is None:
            return "v1replay did not start"
        code = self.process.poll()
        if code is not None:
            self.returncode = code
            return f"V1 emulator exited early with code {code}"
        return ""

    def _bench_completed(self) -> bool:
        try:
            return self.COMPLETE_MARKER in self.log_path.read_bytes()
        except OSError:
            return False

    def _bench_configuration(self) -> dict[str, Any]:
        try:
            lines = self.log_path.read_text(encoding="utf-8", errors="replace").splitlines()
        except OSError:
            return {}
        prefix = "V1REPLAY_EVENT "
        for line in lines:
            if not line.startswith(prefix):
                continue
            try:
                event = json.loads(line[len(prefix) :])
            except json.JSONDecodeError:
                continue
            if event.get("state") == "configured":
                return event
        return {}

    def finish(self, window_completed: bool) -> dict[str, Any]:
        process_was_running = self.process is not None and self.process.poll() is None
        bench_completed = self._bench_completed() if self.mode == "bench" else True
        configuration = self._bench_configuration() if self.mode == "bench" else {}
        try:
            total_samples = int(configuration.get("totalSamples") or 0)
            cadence_hz = int(configuration.get("cadenceHz") or 0)
            blink_samples = int(configuration.get("blinkSamples") or 0)
        except (TypeError, ValueError):
            total_samples = 0
            cadence_hz = 0
            blink_samples = -1
        configuration_valid = self.mode != "bench" or (
            configuration.get("blinkProfile") == self.blink_profile
            and total_samples > 0
            and cadence_hz > 0
            and blink_samples >= 0
        )
        self.completed = bool(
            window_completed and process_was_running and bench_completed and configuration_valid
        )
        if process_was_running:
            self.managed_stop = True
            self.stop()
        elif self.process is not None:
            self.returncode = self.process.poll()
        self._close_log()
        return {
            "started": self.started,
            "completed": self.completed,
            "mode": self.mode,
            "blink_profile": str(configuration.get("blinkProfile") or self.blink_profile),
            "blink_source": str(configuration.get("blinkSource") or ""),
            "blink_samples": blink_samples,
            "blink_nominal_seconds": (blink_samples / cadence_hz) if cadence_hz else 0.0,
            "total_samples": total_samples,
            "cadence_hz": cadence_hz,
            "managed_stop": self.managed_stop,
            "returncode": self.returncode,
            "log": self.log_path.name if self.log_path.is_file() else "",
        }

    def _close_log(self) -> None:
        if self.log_handle is not None:
            self.log_handle.close()
            self.log_handle = None

    def stop(self) -> None:
        if self.process is not None and self.process.poll() is None:
            try:
                os.killpg(self.process.pid, signal.SIGTERM)
                self.process.wait(timeout=2)
            except (OSError, subprocess.TimeoutExpired):
                if self.process.poll() is None:
                    try:
                        os.killpg(self.process.pid, signal.SIGKILL)
                    except OSError:
                        pass
                    try:
                        self.process.wait(timeout=2)
                    except subprocess.TimeoutExpired:
                        pass
        if self.process is not None:
            self.returncode = self.process.poll()
        self._close_log()


def collect_live(
    args: argparse.Namespace,
    out_dir: Path,
) -> tuple[Path, Path | None, dict[str, Any], str, dict[str, Any], dict[str, Any]]:
    port = wait_for_port(args.port)
    if args.upload:
        print("[bench] uploading firmware/filesystem before first window", flush=True)
        run_upload(port, args.skip_web)
        port = wait_for_port(port, 30)
        time.sleep(2)
        wait_for_post_upload_settle(args.post_upload_settle_seconds)

    protocol_log = out_dir / "bench_serial.log"
    q: BenchSerial | None = None
    completion: dict[str, Any] = {}
    emulator = V1Emulator(
        Path(args.replay_executable).resolve(), out_dir, args.suite, args.blink_profile
    )
    emulator_result: dict[str, Any] = {}
    camera = CameraCapture(out_dir / "camera", args.duration_seconds) if args.camera else None
    camera_result: dict[str, Any] = {}
    encounter_csv_path: Path | None = None
    collection_completed = False
    try:
        if camera is not None:
            if camera.start():
                print(f"[bench] camera recording started: {camera.video_path}", flush=True)
            else:
                print(f"[bench] camera capture unavailable; see {camera.result_path}", file=sys.stderr, flush=True)
        print(f"[bench] opening serial port {port}; protocol log: {protocol_log}", flush=True)
        q = BenchSerial(port, args.baud, protocol_log)
        ready = wait_ready(q, args.ready_timeout_seconds)
        print(f"[bench] protocol ready: {ready}", flush=True)
        completion = start_and_wait(
            q,
            args.suite,
            args.duration_seconds,
            args.completion_grace_seconds,
            after_started=emulator.start,
            health_check=emulator.health_problem,
        )
        try:
            csv_path = download_csv(q, out_dir, args.export_idle_timeout_seconds)
        except TimeoutError as exc:
            sd_path = str(completion.get("csvPath") or "")
            if not sd_path:
                raise
            print(f"[bench] export timed out ({exc}); retrying explicit SD path {sd_path}", flush=True)
            q.close()
            q = None
            last_error: Exception | None = exc
            for attempt in range(1, max(0, args.export_retries) + 1):
                try:
                    port = wait_for_port(port, 10)
                    print(f"[bench] recovery export attempt {attempt}/{args.export_retries}", flush=True)
                    q = BenchSerial(port, args.baud, protocol_log)
                    ready = wait_ready(q, args.ready_timeout_seconds)
                    print(f"[bench] recovery protocol ready: {ready}", flush=True)
                    csv_path = download_csv(q, out_dir, args.export_recovery_idle_timeout_seconds, sd_path)
                    break
                except Exception as retry_exc:  # noqa: BLE001 - keep retry evidence
                    last_error = retry_exc
                    if q is not None:
                        q.close()
                        q = None
                    time.sleep(1)
            else:
                raise RuntimeError(f"CSV export recovery failed: {last_error}") from last_error
        if args.suite == "replay":
            encounter_sd_path = encounter_csv_sd_path(str(completion.get("csvPath") or ""))
            if not encounter_sd_path:
                raise RuntimeError("Could not derive encounter CSV path from the perf CSV path")
            encounter_csv_path = download_csv(q, out_dir, args.export_idle_timeout_seconds, encounter_sd_path)
        collection_completed = True
    finally:
        if q is not None:
            q.close()
        if not emulator_result:
            emulator_result = emulator.finish(collection_completed)
        if camera is not None:
            camera_result = camera.stop(collection_completed)
            try:
                camera_result = json.loads(camera.result_path.read_text(encoding="utf-8"))
            except (OSError, json.JSONDecodeError):
                pass
            emulator_start_video_s: float | None = None
            if camera.recording_started_monotonic is not None and emulator.started_monotonic is not None:
                emulator_start_video_s = round(
                    emulator.started_monotonic - camera.recording_started_monotonic, 3
                )
                camera_result["emulator_start_video_seconds"] = emulator_start_video_s
            camera_result["visually_graded"] = False
            camera_result["grade"] = ""
            camera_result["grade_result"] = ""
            camera_grade: dict[str, Any] = {}
            if camera_grade_required(args.suite, camera_result):
                camera_grade = grade_camera(
                    suite=args.suite,
                    camera_dir=camera.out_dir,
                    camera_result=camera_result,
                    emulator_result=emulator_result,
                    encounter_csv_path=encounter_csv_path,
                    emulator_start_video_s=emulator_start_video_s,
                )
                camera_result["visually_graded"] = True
                camera_result["grade"] = "camera_grade.json"
                camera_result["grade_result"] = camera_grade.get("result")
            camera.result_path.write_text(json.dumps(camera_result, indent=2) + "\n", encoding="utf-8")
            grade_result = camera_grade.get("result") or (
                "ungraded" if camera_result.get("result") == "CAPTURED" else "unavailable"
            )
            print(f"[bench] camera capture={camera_result.get('result')} grade={grade_result}", flush=True)
    if not emulator_result.get("completed"):
        mode = str(emulator_result.get("mode") or args.suite)
        raise RuntimeError(f"V1 emulator mode={mode} did not cover the complete metrics window")
    return csv_path, encounter_csv_path, completion, port, emulator_result, camera_result


def camera_grade_required(suite: str, camera_result: dict[str, Any]) -> bool:
    """Only replay has an independent log contract suitable for camera grading."""
    return suite == "replay" and camera_result.get("result") == "CAPTURED"


def main() -> int:
    install_signal_handlers()
    args = parse_args()
    blink_profile_was_selected = args.blink_profile is not None or args.blink_arrow
    if args.blink_arrow:
        args.blink_profile = "stress"
    elif args.blink_profile is None:
        args.blink_profile = "scenario" if args.suite == "replay" else "steady"
    out_dir = Path(args.out_dir).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    if args.duration_seconds < 1:
        write_window_result(
            out_dir,
            {"result": "COLLECTION_FAILED", "suite": args.suite, "error": "duration must be positive"},
        )
        return 3
    if args.post_upload_settle_seconds < 0:
        write_window_result(
            out_dir,
            {
                "result": "COLLECTION_FAILED",
                "suite": args.suite,
                "error": "post-upload settle duration cannot be negative",
            },
        )
        return 3
    if blink_profile_was_selected and args.suite != "replay":
        write_window_result(
            out_dir,
            {
                "result": "COLLECTION_FAILED",
                "suite": args.suite,
                "error": "--blink-profile requires the replay suite",
            },
        )
        return 3
    if not args.from_csv and not args.replay_executable:
        write_window_result(
            out_dir,
            {
                "result": "COLLECTION_FAILED",
                "suite": args.suite,
                "error": "live bench suites require --replay-executable for managed V1 emulation",
            },
        )
        return 3
    if args.from_csv and args.replay_executable:
        write_window_result(
            out_dir,
            {
                "result": "COLLECTION_FAILED",
                "suite": args.suite,
                "error": "--replay-executable cannot be used with --from-csv",
            },
        )
        return 3
    if args.camera and args.from_csv:
        write_window_result(
            out_dir,
            {"result": "COLLECTION_FAILED", "suite": args.suite, "error": "--camera cannot be used with --from-csv"},
        )
        return 3

    try:
        if args.from_csv:
            source = Path(args.from_csv).resolve()
            if not source.is_file():
                raise RuntimeError(f"CSV not found: {source}")
            csv_path = out_dir / source.name
            if source != csv_path:
                shutil.copy2(source, csv_path)
            completion: dict[str, Any] = {"source": "from_csv"}
            port = ""
            emulator_result: dict[str, Any] = {}
            camera_result: dict[str, Any] = {}
            encounter_csv_path: Path | None = None
        else:
            csv_path, encounter_csv_path, completion, port, emulator_result, camera_result = collect_live(args, out_dir)

        import_proc = run_import(args, csv_path, out_dir)
        scoring_path = out_dir / "scoring.json"
        manifest_path = out_dir / "manifest.json"
        result = "COLLECTION_FAILED" if import_proc.returncode >= 3 else "COLLECTED"
        write_window_result(
            out_dir,
            {
                "result": result,
                "suite": args.suite,
                "board_id": args.board_id,
                "git_sha": args.git_sha,
                "git_ref": args.git_ref,
                "git_worktree_clean": args.git_worktree_clean == "1",
                "duration_seconds": args.duration_seconds,
                "post_upload_settle_seconds": args.post_upload_settle_seconds if args.upload else 0,
                "segment": args.segment,
                "port": port,
                "csv_path": str(csv_path),
                "encounter_csv_path": str(encounter_csv_path) if encounter_csv_path else "",
                "completion": completion,
                "v1_emulator": emulator_result,
                "replay": emulator_result if args.suite == "replay" else {},
                "camera": camera_result,
                "import_returncode": import_proc.returncode,
                "manifest_path": str(manifest_path) if manifest_path.exists() else "",
                "scoring_path": str(scoring_path) if scoring_path.exists() else "",
            },
        )
        return 0 if import_proc.returncode < 3 else 3
    except Exception as exc:  # noqa: BLE001 - top-level artifact capture
        write_window_result(
            out_dir,
            {
                "result": "COLLECTION_FAILED",
                "suite": args.suite,
                "board_id": args.board_id,
                "git_sha": args.git_sha,
                "git_ref": args.git_ref,
                "git_worktree_clean": args.git_worktree_clean == "1",
                "error": str(exc),
            },
        )
        print(f"[bench] collection failed: {exc}", file=sys.stderr)
        return 3


if __name__ == "__main__":
    raise SystemExit(main())
