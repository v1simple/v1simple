#!/usr/bin/env python3
"""Measure firmware C++ line coverage for the native (host) test suites.

Runs the native suites under the instrumented `native-coverage` PlatformIO
environment (via scripts/run_native_tests_serial.py, which keeps one
PLATFORMIO_BUILD_DIR per suite so .gcno/.gcda never collide), then aggregates
the per-suite gcov data with gcovr.

MEASURED SCOPE — read this before quoting a number
--------------------------------------------------
The native environments set `test_build_src = false`. A native suite therefore
compiles only the production units and mocks it #includes, not the whole
firmware image. Everything reported here is coverage of *the firmware code the
native suites actually build* — the meaningful denominator for host-testable
logic. It is NOT whole-firmware coverage, and it must never be quoted as such.
Firmware code that only compiles for the device (drivers, Arduino/NimBLE-bound
paths) is out of this denominator and stays covered by the mutation gate and the
device suites, exactly as it is today.

Every run writes an explicit measured-scope file list next to the report so the
denominator is auditable and the headline number cannot be misread.

Usage:
    python3 scripts/run_firmware_coverage.py                 # full native suite
    python3 scripts/run_firmware_coverage.py test_settings   # subset (pipeline check)
    python3 scripts/run_firmware_coverage.py --skip-tests    # re-aggregate existing gcov data
    python3 scripts/run_firmware_coverage.py --write-baseline

Exit codes:
    0 = coverage measured and reported
    1 = test run or aggregation failed
    2 = usage / input error
"""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCRIPTS = ROOT / "scripts"
COVERAGE_ENV = "native-coverage"
BUILD_ROOT = ROOT / ".artifacts" / "serial_test_builds" / COVERAGE_ENV
REPORT_DIR = ROOT / ".artifacts" / "test_reports" / "coverage"
BASELINE_PATH = ROOT / "test" / "contracts" / "coverage_baseline.json"

REPORT_JSON = "coverage.json"
REPORT_SUMMARY_JSON = "gcovr_summary.json"
REPORT_HTML = "coverage.html"
REPORT_SCOPE = "coverage_scope.txt"

# Only firmware sources count. gcovr filters are regexes resolved against --root.
COVERAGE_FILTERS = ("^src/", "^include/")

# Generated data blobs and vendored font tables are not hand-written logic: they
# are constant-array headers that would dilute the ratchet into noise.
# NOTE the `(^|.*/)` anchor: gcovr resolves relative patterns against --root, so
# a bare `.*/include/foo\.h` would NOT match the root-relative `include/foo.h`.
COVERAGE_EXCLUDES = (
    r"(^|.*/)test/.*",
    r"(^|.*/)\.pio/.*",
    r"(^|.*/)include/v1simple_logo\.h$",
    r"(^|.*/)include/warning_audio\.h$",
    r"(^|.*/)include/Segment7Font\.h$",
    r"(^|.*/)include/FreeSans.*\.h$",
)

DEFAULT_TOLERANCE_PP = 0.5


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    parser.add_argument(
        "tests",
        nargs="*",
        help=(
            "Optional native test directory names. Default: the whole native "
            "suite. A subset proves the pipeline but produces a subset-scoped "
            "report, which is NOT a valid whole-suite baseline."
        ),
    )
    parser.add_argument(
        "--skip-tests",
        action="store_true",
        help="Aggregate the gcov data already under the build root; do not re-run tests.",
    )
    parser.add_argument(
        "--write-baseline",
        action="store_true",
        help=f"Write the measured report to {BASELINE_PATH.relative_to(ROOT)}.",
    )
    parser.add_argument(
        "--report-dir",
        type=Path,
        default=REPORT_DIR,
        help=f"Output directory (default: {REPORT_DIR.relative_to(ROOT)}).",
    )
    parser.add_argument(
        "--tolerance-pp",
        type=float,
        default=DEFAULT_TOLERANCE_PP,
        help=(
            "Ratchet tolerance in percentage points recorded into a written "
            f"baseline (default: {DEFAULT_TOLERANCE_PP})."
        ),
    )
    return parser.parse_args()


def run_instrumented_suites(tests: list[str]) -> tuple[int, float]:
    """Run the native suites under native-coverage, one suite per build dir."""
    command = [
        sys.executable,
        str(SCRIPTS / "run_native_tests_serial.py"),
        "--env",
        COVERAGE_ENV,
        *tests,
    ]
    print(f"[firmware-coverage] running instrumented suites: {' '.join(command)}")
    started = time.monotonic()
    result = subprocess.run(command, cwd=ROOT)
    elapsed = time.monotonic() - started
    return result.returncode, elapsed


def gcovr_command(report_dir: Path) -> list[str]:
    command = [
        sys.executable,
        "-m",
        "gcovr",
        "--root",
        str(ROOT),
        str(BUILD_ROOT),
        "--gcov-ignore-parse-errors",
        "--merge-mode-functions",
        "merge-use-line-min",
        "--exclude-noncode-lines",
        "--json-summary-pretty",
        "--json-summary",
        str(report_dir / REPORT_SUMMARY_JSON),
        "--html-details",
        str(report_dir / REPORT_HTML),
        "--html-title",
        "v1simple native-suite firmware coverage",
    ]
    for pattern in COVERAGE_FILTERS:
        command += ["--filter", pattern]
    for pattern in COVERAGE_EXCLUDES:
        command += ["--exclude", pattern]
    return command


def aggregate(report_dir: Path) -> dict:
    command = gcovr_command(report_dir)
    print(f"[firmware-coverage] aggregating: {' '.join(command)}")
    result = subprocess.run(command, cwd=ROOT)
    if result.returncode != 0:
        raise RuntimeError(f"gcovr failed with exit {result.returncode}")

    summary_path = report_dir / REPORT_SUMMARY_JSON
    if not summary_path.exists():
        raise RuntimeError(f"gcovr produced no summary at {summary_path}")
    with open(summary_path, encoding="utf-8") as handle:
        return json.load(handle)


def normalize(summary: dict, *, suites: list[str], full_suite: bool, tolerance: float) -> dict:
    """Reshape the gcovr summary into the schema the ratchet checker consumes."""
    files: dict[str, dict[str, float | int]] = {}
    for entry in summary.get("files", []):
        filename = str(entry["filename"]).replace("\\", "/")
        files[filename] = {
            "line_percent": round(float(entry.get("line_percent", 0.0)), 2),
            "line_total": int(entry.get("line_total", 0)),
            "line_covered": int(entry.get("line_covered", 0)),
        }

    return {
        "schema": "v1simple.firmware-coverage/1",
        "scope": {
            "description": (
                "Native (host) test suites only. test_build_src = false, so this "
                "measures the firmware code the native suites actually compile, "
                "NOT the whole firmware image. Device-only driver code is outside "
                "this denominator and is covered by the mutation gate and the "
                "device suites."
            ),
            "environment": COVERAGE_ENV,
            "full_suite": full_suite,
            "suite_count": len(suites),
            "suites": suites,
            "measured_file_count": len(files),
            "filters": list(COVERAGE_FILTERS),
            "excludes": list(COVERAGE_EXCLUDES),
        },
        "tolerance_pp": tolerance,
        "overall": {
            "line_percent": round(float(summary.get("line_percent", 0.0)), 2),
            "line_total": int(summary.get("line_total", 0)),
            "line_covered": int(summary.get("line_covered", 0)),
        },
        "files": dict(sorted(files.items())),
    }


def write_scope_list(report: dict, path: Path) -> None:
    scope = report["scope"]
    overall = report["overall"]
    lines = [
        "v1simple firmware coverage — measured scope",
        "===========================================",
        "",
        "HOW TO READ THIS NUMBER",
        "-----------------------",
        "The native PlatformIO environments set test_build_src = false. Each native",
        "suite compiles only the production units and mocks it #includes, so the",
        "denominator below is the firmware code the native suites actually build.",
        "This is NOT whole-firmware coverage and must not be quoted as such.",
        "Device-only driver code is outside this denominator and remains covered by",
        "the mutation gate and the device test suites.",
        "",
        f"environment          : {scope['environment']}",
        f"suites executed      : {scope['suite_count']}"
        + ("" if scope["full_suite"] else "  (SUBSET — not a valid baseline)"),
        f"full native suite    : {'yes' if scope['full_suite'] else 'NO'}",
        f"measured files       : {scope['measured_file_count']}",
        f"overall line coverage: {overall['line_percent']:.2f}% "
        f"({overall['line_covered']}/{overall['line_total']} lines)",
        "",
        "MEASURED FILES (the complete denominator)",
        "-----------------------------------------",
    ]
    width = max((len(name) for name in report["files"]), default=0)
    for name, entry in report["files"].items():
        lines.append(
            f"{name.ljust(width)}  {entry['line_percent']:6.2f}%  "
            f"({entry['line_covered']}/{entry['line_total']} lines)"
        )
    lines.append("")
    lines.append("EXCLUDED FROM SCOPE")
    lines.append("-------------------")
    lines.append("Tests and mocks (test/), library deps (.pio/), and generated data")
    lines.append("headers: v1simple_logo.h, warning_audio.h, Segment7Font.h, FreeSans*.h")
    lines.append("")
    path.write_text("\n".join(lines), encoding="utf-8")


def baseline_document(report: dict) -> dict:
    return {
        "schema": "v1simple.firmware-coverage-baseline/1",
        "comment": (
            "Ratchet baseline for scripts/check_firmware_coverage.py. Regenerate "
            "with: python3 scripts/run_firmware_coverage.py --write-baseline "
            "(a FULL native-suite run — a subset run is not a valid baseline). "
            "Scope: native host suites only; test_build_src = false, so this is "
            "coverage of the firmware code the native suites compile, not the "
            "whole firmware image."
        ),
        "scope": report["scope"],
        "tolerance_pp": report["tolerance_pp"],
        "overall": report["overall"],
        "files": report["files"],
    }


def main() -> int:
    args = parse_args()
    report_dir: Path = args.report_dir
    report_dir.mkdir(parents=True, exist_ok=True)

    if shutil.which("gcov") is None:
        print("[firmware-coverage] gcov not found in PATH", file=sys.stderr)
        return 2

    test_seconds = 0.0
    if args.skip_tests:
        if not BUILD_ROOT.exists():
            print(
                f"[firmware-coverage] --skip-tests requires existing gcov data under "
                f"{BUILD_ROOT}",
                file=sys.stderr,
            )
            return 2
        print("[firmware-coverage] --skip-tests: reusing existing gcov data")
    else:
        returncode, test_seconds = run_instrumented_suites(args.tests)
        if returncode != 0:
            print(
                f"[firmware-coverage] instrumented suite run failed (exit {returncode})",
                file=sys.stderr,
            )
            return 1

    executed = sorted(path.name for path in BUILD_ROOT.iterdir() if path.is_dir())
    if not executed:
        print(f"[firmware-coverage] no per-suite build dirs under {BUILD_ROOT}", file=sys.stderr)
        return 1

    # A run is only "full" if it executed every suite the runner would discover.
    sys.path.insert(0, str(SCRIPTS))
    from run_native_tests_serial import discover_native_tests

    full_suite = set(executed) >= set(discover_native_tests(COVERAGE_ENV))

    try:
        summary = aggregate(report_dir)
    except RuntimeError as exc:
        print(f"[firmware-coverage] {exc}", file=sys.stderr)
        return 1

    report = normalize(
        summary,
        suites=executed,
        full_suite=full_suite,
        tolerance=args.tolerance_pp,
    )
    if not report["files"]:
        print(
            "[firmware-coverage] no src/ or include/ files were measured — the "
            "gcov data or the filters are wrong",
            file=sys.stderr,
        )
        return 1

    # A silently mis-anchored exclude regex would quietly change the denominator,
    # which is exactly the failure mode that makes a coverage number a lie.
    leaked = sorted(
        name
        for name in report["files"]
        if not name.startswith(("src/", "include/"))
        or any(
            token in name
            for token in ("v1simple_logo", "warning_audio", "Segment7Font", "FreeSans")
        )
    )
    if leaked:
        print(
            "[firmware-coverage] filters leaked out-of-scope file(s) into the "
            "denominator:",
            file=sys.stderr,
        )
        for name in leaked:
            print(f"  - {name}", file=sys.stderr)
        return 1

    report_path = report_dir / REPORT_JSON
    with open(report_path, "w", encoding="utf-8") as handle:
        json.dump(report, handle, indent=2, sort_keys=False)
        handle.write("\n")
    write_scope_list(report, report_dir / REPORT_SCOPE)

    if args.write_baseline:
        BASELINE_PATH.parent.mkdir(parents=True, exist_ok=True)
        with open(BASELINE_PATH, "w", encoding="utf-8") as handle:
            json.dump(baseline_document(report), handle, indent=2, sort_keys=False)
            handle.write("\n")
        print(f"[firmware-coverage] wrote baseline {BASELINE_PATH.relative_to(ROOT)}")
        if not full_suite:
            print(
                "[firmware-coverage] WARNING: baseline written from a SUBSET run "
                "and is not a trustworthy ratchet floor",
                file=sys.stderr,
            )

    overall = report["overall"]
    print("")
    print(
        f"[firmware-coverage] overall {overall['line_percent']:.2f}% lines "
        f"({overall['line_covered']}/{overall['line_total']}) across "
        f"{report['scope']['measured_file_count']} measured file(s) from "
        f"{len(executed)} suite(s)"
    )
    print(
        "[firmware-coverage] scope: native host suites only (test_build_src = false) "
        "— not whole-firmware coverage"
    )
    if not full_suite:
        print("[firmware-coverage] SUBSET run — not a valid baseline")
    if test_seconds:
        print(f"[firmware-coverage] instrumented suite wall clock: {test_seconds:.1f}s")
    print(f"[firmware-coverage] report   {(report_dir / REPORT_JSON).relative_to(ROOT)}")
    print(f"[firmware-coverage] html     {(report_dir / REPORT_HTML).relative_to(ROOT)}")
    print(f"[firmware-coverage] scope    {(report_dir / REPORT_SCOPE).relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
