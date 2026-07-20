#!/usr/bin/env python3
"""Collect one SD-backed bench window over USB serial.

This is intentionally only a collector/importer. It starts one firmware bench
window (core or display), downloads the SD perf CSV, imports it with the shared
CSV importer, and writes a small window_result.json. It does not decide release
release verdicts, apply OBD/proxy coverage, or promote baselines; optional
baseline manifests are passed through only for importer comparison output.
"""

from __future__ import annotations

import argparse
import binascii
import glob
import json
import os
import shutil
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

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
    parser.add_argument("--suite", choices=["core", "display"], required=True)
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
    parser.add_argument("--from-csv", default="", help="Import an existing perf CSV instead of collecting live")
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


def start_and_wait(q: BenchSerial, suite: str, duration_s: int, grace_s: int) -> dict[str, Any]:
    start_deadline = time.monotonic() + 15
    last_start_error: dict[str, Any] | None = None
    start_payload: dict[str, Any] | None = None
    command = f"QSTART {suite} {duration_s}"
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
                if payload.get("ok") and payload.get("state") == "running" and payload.get("suite") == suite:
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

    deadline = time.monotonic() + duration_s + grace_s
    run_started = time.monotonic()
    next_progress = run_started + RUN_PROGRESS_INTERVAL_S
    last_event: dict[str, Any] = start_payload
    while time.monotonic() < deadline:
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


def run_import(args: argparse.Namespace, csv_path: Path, out_dir: Path) -> subprocess.CompletedProcess[str]:
    stress_class = "core" if args.suite == "core" else "display_preview"
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


def collect_live(args: argparse.Namespace, out_dir: Path) -> tuple[Path, dict[str, Any], str]:
    port = wait_for_port(args.port)
    if args.upload:
        print("[bench] uploading firmware/filesystem before first window", flush=True)
        run_upload(port, args.skip_web)
        port = wait_for_port(port, 30)
        time.sleep(2)

    protocol_log = out_dir / "bench_serial.log"
    q: BenchSerial | None = None
    completion: dict[str, Any] = {}
    try:
        print(f"[bench] opening serial port {port}; protocol log: {protocol_log}", flush=True)
        q = BenchSerial(port, args.baud, protocol_log)
        ready = wait_ready(q, args.ready_timeout_seconds)
        print(f"[bench] protocol ready: {ready}", flush=True)
        completion = start_and_wait(q, args.suite, args.duration_seconds, args.completion_grace_seconds)
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
    finally:
        if q is not None:
            q.close()
    return csv_path, completion, port


def main() -> int:
    args = parse_args()
    out_dir = Path(args.out_dir).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    if args.duration_seconds < 1:
        write_window_result(out_dir, {"result": "COLLECTION_FAILED", "suite": args.suite, "error": "duration must be positive"})
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
        else:
            csv_path, completion, port = collect_live(args, out_dir)

        import_proc = run_import(args, csv_path, out_dir)
        scoring_path = out_dir / "scoring.json"
        manifest_path = out_dir / "manifest.json"
        csv_scorecard_path = out_dir / "csv_scorecard.json"
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
                "port": port,
                "csv_path": str(csv_path),
                "completion": completion,
                "import_returncode": import_proc.returncode,
                "manifest_path": str(manifest_path) if manifest_path.exists() else "",
                "scoring_path": str(scoring_path) if scoring_path.exists() else "",
                "csv_scorecard_path": str(csv_scorecard_path) if csv_scorecard_path.exists() else "",
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
