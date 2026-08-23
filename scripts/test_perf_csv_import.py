#!/usr/bin/env python3
"""Focused regression tests for raw perf CSV validation."""

from __future__ import annotations

import csv
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TOOLS_DIR = ROOT / "tools"
sys.path.insert(0, str(TOOLS_DIR))

import import_perf_csv  # type: ignore  # noqa: E402


REPLAY_COLUMNS = [
    "millis",
    "utc",
    *import_perf_csv.REPLAY_EXACT_DELTAS,
    *import_perf_csv.REPLAY_ZERO_DELTAS,
    import_perf_csv.REPLAY_ALL_VOLUME_COUNTER,
]


def assert_true(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def replay_row(millis: int, **overrides: int) -> dict[str, int | str]:
    row: dict[str, int | str] = {column: 0 for column in REPLAY_COLUMNS}
    row["millis"] = millis
    row["utc"] = "2026-08-23T12:00:00Z"
    row.update(overrides)
    return row


def write_replay_csv(
    path: Path,
    *,
    final_overrides: dict[str, int] | None = None,
    replacement_seq: int = 2,
    omit_column: str = "",
) -> None:
    columns = [column for column in REPLAY_COLUMNS if column != omit_column]
    final = replay_row(
        250,
        prioritySelectRowFlag=708,
        alertTablePublishes=708,
        alertTablePublishes3Bogey=30,
        v1AllVolumeParsed=1,
    )
    final.update(final_overrides or {})
    sessions = [
        (
            "#session_start,seq=1,bootId=9,uptime_ms=100,token=AAAA,schema=46",
            [replay_row(100), replay_row(150)],
        ),
        (
            f"#session_start,seq={replacement_seq},bootId=9,uptime_ms=200,token=BBBB,schema=46",
            [replay_row(200), final],
        ),
    ]
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle, lineterminator="\n")
        for marker, rows in sessions:
            writer.writerow(columns)
            handle.write(marker + "," * max(0, len(columns) - 6) + "\n")
            for row in rows:
                writer.writerow([row[column] for column in columns])


def run_cli(path: Path, suite: str = "replay", segment: str = "last") -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [
            sys.executable,
            str(ROOT / "tools" / "import_perf_csv.py"),
            "--input",
            str(path),
            "--suite",
            suite,
            "--segment",
            segment,
        ],
        cwd=ROOT,
        text=True,
        capture_output=True,
        check=False,
    )


def test_firmware_session_marker_uses_validator_boot_id_key() -> None:
    source = (ROOT / "src" / "perf_sd_logger.cpp").read_text(encoding="utf-8")
    assert_true(
        '"#session_start,seq=%lu,bootId=%lu,' in source,
        "firmware marker must emit the bootId key consumed by the validator",
    )


def test_raw_parser_preserves_sessions_and_skips_utc() -> None:
    with tempfile.TemporaryDirectory() as raw:
        path = Path(raw) / "perf.csv"
        write_replay_csv(path)
        sessions = import_perf_csv.load_sessions(path)
        assert_true(len(sessions) == 2, f"expected two sessions: {sessions}")
        first_meta, first_rows = sessions[0]
        assert_true(first_meta is not None and first_meta.bootId == 9, str(first_meta))
        assert_true("utc" not in first_rows[0], f"UTC was treated as an integer: {first_rows[0]}")
        assert_true(first_rows[0]["millis"] == 100, str(first_rows[0]))


def test_leading_rows_form_an_implicit_session() -> None:
    with tempfile.TemporaryDirectory() as raw:
        path = Path(raw) / "legacy.csv"
        path.write_text("10,1\n" "millis,rx\n" "20,2\n", encoding="utf-8")
        sessions = import_perf_csv.load_sessions(path)
        assert_true(len(sessions) == 2, f"leading rows were lost: {sessions}")
        assert_true(sessions[0] == (None, [{"millis": 10, "rx": 1}]), str(sessions[0]))


def test_truncated_and_duplicate_rows_are_rejected() -> None:
    with tempfile.TemporaryDirectory() as raw:
        root = Path(raw)
        truncated = root / "truncated.csv"
        truncated.write_text("millis,rx,qDrop\n10,1\n", encoding="utf-8")
        try:
            import_perf_csv.load_sessions(truncated)
        except import_perf_csv.CsvEvidenceError as exc:
            assert_true("expected 3" in str(exc), str(exc))
        else:
            raise AssertionError("truncated raw row was accepted")

        duplicate = root / "duplicate.csv"
        duplicate.write_text("millis,rx,rx\n10,1,1\n", encoding="utf-8")
        try:
            import_perf_csv.load_sessions(duplicate)
        except import_perf_csv.CsvEvidenceError as exc:
            assert_true("repeats columns: rx" in str(exc), str(exc))
        else:
            raise AssertionError("duplicate raw header was accepted")


def test_replay_pass_is_direct_and_writes_nothing() -> None:
    with tempfile.TemporaryDirectory() as raw:
        root = Path(raw)
        path = root / "perf.csv"
        write_replay_csv(path)
        before = sorted(item.name for item in root.iterdir())
        completed = run_cli(path)
        after = sorted(item.name for item in root.iterdir())
        assert_true(completed.returncode == 0, completed.stdout + completed.stderr)
        assert_true(completed.stdout.startswith("PASS: raw replay perf CSV"), completed.stdout)
        assert_true(completed.stderr == "", completed.stderr)
        assert_true(before == after == ["perf.csv"], f"validator wrote derived files: {after}")


def test_replay_semantic_failure_returns_two() -> None:
    with tempfile.TemporaryDirectory() as raw:
        path = Path(raw) / "perf.csv"
        write_replay_csv(path, final_overrides={"parseFail": 1})
        completed = run_cli(path)
        assert_true(completed.returncode == 2, completed.stdout + completed.stderr)
        assert_true(
            completed.stderr.startswith("FAIL (semantic): parseFail delta=1 expected=0"),
            completed.stderr,
        )


def test_replay_collection_failures_return_three() -> None:
    with tempfile.TemporaryDirectory() as raw:
        root = Path(raw)

        missing = root / "missing.csv"
        write_replay_csv(missing, omit_column="qDrop")
        missing_result = run_cli(missing)
        assert_true(missing_result.returncode == 3, missing_result.stderr)
        assert_true("missing required columns: qDrop" in missing_result.stderr, missing_result.stderr)

        discontinuous = root / "discontinuous.csv"
        write_replay_csv(discontinuous, replacement_seq=3)
        discontinuous_result = run_cli(discontinuous)
        assert_true(discontinuous_result.returncode == 3, discontinuous_result.stderr)
        assert_true("metadata is discontinuous" in discontinuous_result.stderr, discontinuous_result.stderr)

        incomplete = root / "incomplete.csv"
        write_replay_csv(incomplete)
        incomplete.write_text(incomplete.read_text(encoding="utf-8").rstrip("\n"), encoding="utf-8")
        incomplete_result = run_cli(incomplete)
        assert_true(incomplete_result.returncode == 3, incomplete_result.stderr)
        assert_true("does not end with a complete line" in incomplete_result.stderr, incomplete_result.stderr)

        empty_counter = root / "empty-counter.csv"
        write_replay_csv(empty_counter)
        text = empty_counter.read_text(encoding="utf-8")
        empty_counter.write_text(
            text.replace(",708,708,30,", ",,708,30,", 1),
            encoding="utf-8",
        )
        empty_result = run_cli(empty_counter)
        assert_true(empty_result.returncode == 3, empty_result.stderr)
        assert_true("is not an integer" in empty_result.stderr, empty_result.stderr)


def test_core_segment_selection_is_structural_only() -> None:
    with tempfile.TemporaryDirectory() as raw:
        path = Path(raw) / "perf.csv"
        write_replay_csv(path)
        completed = run_cli(path, suite="core", segment="1")
        assert_true(completed.returncode == 0, completed.stdout + completed.stderr)
        assert_true("raw core perf CSV segment 1 is structurally valid" in completed.stdout, completed.stdout)


def main() -> int:
    test_firmware_session_marker_uses_validator_boot_id_key()
    test_raw_parser_preserves_sessions_and_skips_utc()
    test_leading_rows_form_an_implicit_session()
    test_truncated_and_duplicate_rows_are_rejected()
    test_replay_pass_is_direct_and_writes_nothing()
    test_replay_semantic_failure_returns_two()
    test_replay_collection_failures_return_three()
    test_core_segment_selection_is_structural_only()
    print("raw perf CSV validation tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
