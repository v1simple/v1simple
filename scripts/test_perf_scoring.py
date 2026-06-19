#!/usr/bin/env python3
"""Deterministic regression tests for perf CSV scoring and session selection."""

from __future__ import annotations

import csv
import json
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TOOLS_DIR = ROOT / "tools"
sys.path.insert(0, str(TOOLS_DIR))

import score_perf_csv  # type: ignore  # noqa: E402


HEADER_COLUMNS = [
    line.strip()
    for line in (ROOT / "test" / "contracts" / "perf_csv_column_contract.txt").read_text(encoding="utf-8").splitlines()
    if line.strip() and not line.startswith("#")
]


def assert_true(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def base_row(millis: int, *, connected: bool, duration_ready: bool = False) -> dict[str, int]:
    row = {column: 0 for column in HEADER_COLUMNS}
    row.update(
        {
            "millis": millis,
            "qDrop": 0,
            "parseFail": 0,
            "parseResync": 0,
            "oversizeDrops": 0,
            "bleMutexTimeout": 0,
            "loopMax_us": 100000,
            "bleDrainMax_us": 4000,
            "bleProcessMax_us": 40000,
            "dispPipeMax_us": 30000,
            "flushMax_us": 40000,
            "sdMax_us": 20000,
            "fsMax_us": 20000,
            "queueHighWater": 5,
            "dmaLargestMin": 15000,
            "dmaFreeMin": 30000,
            "wifiConnectDeferred": 0,
            "wifiMax_us": 500,
            "displayUpdates": 0,
            "displaySkips": 0,
            "cmdPaceNotYet": 0,
            "reconn": 0,
            "disc": 0,
            "rx": 0,
            "parseOK": 0,
        }
    )
    if connected and duration_ready:
        row["rx"] = 120
        row["parseOK"] = 120
    return row


def make_session(
    *,
    seq: int,
    token: str,
    duration_ms: int,
    connected: bool,
    end_overrides: dict[str, int] | None = None,
) -> dict[str, object]:
    start = base_row(0, connected=connected, duration_ready=False)
    end = base_row(duration_ms, connected=connected, duration_ready=True)
    if end_overrides:
        end.update(end_overrides)
    return {
        "meta": f"#session_start,seq={seq},bootId={seq},uptime_ms={duration_ms},token={token},schema=9",
        "rows": [start, end],
    }


def write_capture(path: Path, sessions: list[dict[str, object]]) -> None:
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=HEADER_COLUMNS)
        for session in sessions:
            writer.writeheader()
            handle.write(f"{session['meta']}\n")
            for row in session["rows"]:
                writer.writerow(row)


def run_score(csv_path: Path, profile: str, session: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [
            sys.executable,
            str(ROOT / "tools" / "score_perf_csv.py"),
            str(csv_path),
            "--profile",
            profile,
            "--session",
            session,
        ],
        cwd=ROOT,
        text=True,
        capture_output=True,
        check=False,
    )


def run_compare(
    baseline_path: Path,
    candidate_path: Path,
    profile: str,
    session: str,
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [
            sys.executable,
            str(ROOT / "tools" / "compare_perf_csv.py"),
            str(baseline_path),
            str(candidate_path),
            "--profile",
            profile,
            "--session",
            session,
        ],
        cwd=ROOT,
        text=True,
        capture_output=True,
        check=False,
    )


def test_pass_warn_fail_and_error(tmpdir: Path) -> None:
    pass_csv = tmpdir / "pass.csv"
    warn_csv = tmpdir / "warn.csv"
    fail_csv = tmpdir / "fail.csv"
    bad_csv = tmpdir / "bad.csv"

    write_capture(
        pass_csv,
        [make_session(seq=1, token="PASS0001", duration_ms=60000, connected=True)],
    )
    write_capture(
        warn_csv,
        [
            make_session(
                seq=1,
                token="WARN0001",
                duration_ms=60000,
                connected=True,
                end_overrides={"displayUpdates": 100, "displaySkips": 30},
            )
        ],
    )
    write_capture(
        fail_csv,
        [
            make_session(
                seq=1,
                token="FAIL0001",
                duration_ms=60000,
                connected=True,
                end_overrides={"queueHighWater": 13},
            )
        ],
    )
    bad_csv.write_text("not,a,perf,csv\n", encoding="utf-8")

    assert_true(run_score(pass_csv, "drive_wifi_off", "1").returncode == 0, "hard pass must exit 0")
    assert_true(run_score(warn_csv, "drive_wifi_off", "1").returncode == 1, "advisory warning must exit 1")
    assert_true(run_score(fail_csv, "drive_wifi_off", "1").returncode == 2, "hard fail must exit 2")
    assert_true(run_score(bad_csv, "drive_wifi_off", "1").returncode == 3, "malformed capture must exit 3")


def test_session_selection_behavior(tmpdir: Path) -> None:
    multi_csv = tmpdir / "multi.csv"
    write_capture(
        multi_csv,
        [
            make_session(seq=1, token="SESS0001", duration_ms=20000, connected=True),
            make_session(seq=2, token="SESS0002", duration_ms=40000, connected=True),
            make_session(seq=3, token="SESS0003", duration_ms=60000, connected=False),
        ],
    )

    sessions = score_perf_csv.load_sessions(multi_csv)
    meta_1, _rows_1, index_1 = score_perf_csv.select_session(sessions, "1")
    meta_last, _rows_last, index_last = score_perf_csv.select_session(sessions, "last")
    meta_last_connected, _rows_last_connected, index_last_connected = score_perf_csv.select_session(
        sessions, "last-connected"
    )
    meta_longest, _rows_longest, index_longest = score_perf_csv.select_session(sessions, "longest")
    meta_longest_connected, _rows_longest_connected, index_longest_connected = score_perf_csv.select_session(
        sessions, "longest-connected"
    )

    assert_true(index_1 == 1 and meta_1 and meta_1.token == "SESS0001", "selector '1' must choose session 1")
    assert_true(index_last == 3 and meta_last and meta_last.token == "SESS0003", "selector 'last' must choose final session")
    assert_true(
        index_last_connected == 2 and meta_last_connected and meta_last_connected.token == "SESS0002",
        "selector 'last-connected' must choose the final connected session",
    )
    assert_true(index_longest == 3 and meta_longest and meta_longest.token == "SESS0003", "selector 'longest' must choose the longest duration session")
    assert_true(
        index_longest_connected == 2 and meta_longest_connected and meta_longest_connected.token == "SESS0002",
        "selector 'longest-connected' must choose the longest connected session",
    )


def test_no_connected_session_failure(tmpdir: Path) -> None:
    idle_csv = tmpdir / "idle.csv"
    write_capture(
        idle_csv,
        [
            make_session(seq=1, token="IDLE0001", duration_ms=20000, connected=False),
            make_session(seq=2, token="IDLE0002", duration_ms=40000, connected=False),
        ],
    )
    result = run_score(idle_csv, "drive_wifi_off", "longest-connected")
    assert_true(result.returncode == 3, "longest-connected must fail when no connected sessions exist")


def test_pinned_session_stability(tmpdir: Path) -> None:
    stable_csv = tmpdir / "stable.csv"
    write_capture(
        stable_csv,
        [make_session(seq=1, token="PINN0001", duration_ms=60000, connected=True)],
    )
    pinned_before = run_score(stable_csv, "drive_wifi_off", "1")
    assert_true(pinned_before.returncode == 0, "baseline pinned session must pass")

    write_capture(
        stable_csv,
        [
            make_session(seq=1, token="PINN0001", duration_ms=60000, connected=True),
            make_session(
                seq=2,
                token="PINN0002",
                duration_ms=60000,
                connected=True,
                end_overrides={"queueHighWater": 13},
            ),
        ],
    )

    pinned_after = run_score(stable_csv, "drive_wifi_off", "1")
    last_connected = run_score(stable_csv, "drive_wifi_off", "last-connected")
    assert_true(pinned_after.returncode == 0, "pinned session must remain stable after later sessions are appended")
    assert_true(last_connected.returncode == 2, "last-connected should reflect the appended failing session")


def test_compare_perf_csv_regressions(tmpdir: Path) -> None:
    baseline_csv = tmpdir / "baseline.csv"
    advisory_csv = tmpdir / "advisory.csv"
    hard_csv = tmpdir / "hard.csv"

    write_capture(
        baseline_csv,
        [make_session(seq=1, token="BASE0001", duration_ms=60000, connected=True)],
    )
    write_capture(
        advisory_csv,
        [
            make_session(
                seq=1,
                token="ADVR0001",
                duration_ms=60000,
                connected=True,
                end_overrides={"displayUpdates": 100, "displaySkips": 30},
            )
        ],
    )
    write_capture(
        hard_csv,
        [
            make_session(
                seq=1,
                token="HARD0001",
                duration_ms=60000,
                connected=True,
                end_overrides={"queueHighWater": 13},
            )
        ],
    )

    advisory_result = run_compare(baseline_csv, advisory_csv, "drive_wifi_off", "1")
    hard_result = run_compare(baseline_csv, hard_csv, "drive_wifi_off", "1")

    assert_true(advisory_result.returncode == 1, "advisory regression must exit 1")
    assert_true(hard_result.returncode == 2, "hard regression must exit 2")


def test_reduced_perf_boot_fixture_preserves_failure_profile() -> None:
    csv_path = ROOT / "test" / "fixtures" / "perf" / "perf_boot_6_reduced.csv"
    result = subprocess.run(
        [
            sys.executable,
            str(ROOT / "tools" / "score_perf_csv.py"),
            str(csv_path),
            "--profile",
            "drive_wifi_ap",
            "--session",
            "1",
            "--json",
        ],
        cwd=ROOT,
        text=True,
        capture_output=True,
        check=False,
    )
    assert_true(result.returncode == 2, f"reduced real fixture must remain a hard fail: {result.stderr}")
    payload = json.loads(result.stdout)
    failing_metrics = {
        check["metric"]
        for check in payload["checks"]
        if check["level"] == "hard" and not check["passed"]
    }
    assert_true(
        failing_metrics == {"loopMax_us", "fsMax_us", "queueHighWater", "wifiMax_us"},
        f"unexpected hard-fail metric set for reduced fixture: {failing_metrics}",
    )


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="perf_scoring_") as tmp:
        tmpdir = Path(tmp)
        test_pass_warn_fail_and_error(tmpdir)
        test_session_selection_behavior(tmpdir)
        test_no_connected_session_failure(tmpdir)
        test_pinned_session_stability(tmpdir)
        test_compare_perf_csv_regressions(tmpdir)
        test_reduced_perf_boot_fixture_preserves_failure_profile()

    print("[perf-scoring] pass/warn/fail/error cases verified")
    print("[perf-scoring] session selectors and pinned-session stability verified")
    print("[perf-scoring] compare_perf_csv regression exit codes verified")
    print("[perf-scoring] reduced real fixture failure profile verified")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as exc:
        print(f"[perf-scoring] FAILED: {exc}", file=sys.stderr)
        raise SystemExit(1)
