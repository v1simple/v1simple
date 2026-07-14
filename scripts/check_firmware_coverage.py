#!/usr/bin/env python3
"""Ratchet firmware C++ coverage for the native (host) test suites.

Compares the measured report from scripts/run_firmware_coverage.py against the
tracked baseline in test/contracts/coverage_baseline.json. Coverage may rise
freely; it may not fall more than the baseline's tolerance (default 0.5
percentage points) overall or on any tracked file.

SCOPE: the native environments set test_build_src = false, so both the report and
the baseline describe the firmware code the native suites actually compile — not
whole-firmware coverage. Device-only driver code is outside this denominator and
stays covered by the mutation gate and the device suites.

Usage:
    python3 scripts/check_firmware_coverage.py [report-json] [--baseline PATH]

With no arguments the default report path is used:
    .artifacts/test_reports/coverage/coverage.json

Exit codes:
    0 = at or above baseline (within tolerance)
    1 = coverage regressed below baseline
    2 = usage / input error (missing or malformed report or baseline)
"""

from __future__ import annotations

import argparse
import json
import sys
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_REPORT = ROOT / ".artifacts" / "test_reports" / "coverage" / "coverage.json"
DEFAULT_BASELINE = ROOT / "test" / "contracts" / "coverage_baseline.json"
DEFAULT_TOLERANCE_PP = 0.5

PREFIX = "[firmware-coverage]"


class CoverageContractError(Exception):
    """Raised when a report or baseline document cannot be used."""


@dataclass
class Regression:
    name: str
    baseline: float
    measured: float

    @property
    def delta(self) -> float:
        return self.measured - self.baseline

    def describe(self) -> str:
        return (
            f"{self.name}: {self.measured:.2f}% vs baseline {self.baseline:.2f}% "
            f"({self.delta:+.2f} pp)"
        )


@dataclass
class Verdict:
    tolerance_pp: float
    overall_baseline: float
    overall_measured: float
    regressions: list[Regression] = field(default_factory=list)
    improvements: list[Regression] = field(default_factory=list)
    missing_files: list[str] = field(default_factory=list)
    new_files: list[str] = field(default_factory=list)

    @property
    def failed(self) -> bool:
        return bool(self.regressions) or bool(self.missing_files)

    @property
    def rose(self) -> bool:
        return bool(self.improvements) or bool(self.new_files)


def load_document(path: Path, label: str) -> dict:
    if not path.exists():
        raise CoverageContractError(f"{label} not found: {path}")
    try:
        with open(path, encoding="utf-8") as handle:
            document = json.load(handle)
    except json.JSONDecodeError as exc:
        raise CoverageContractError(f"{label} is not valid JSON: {path}: {exc}") from exc
    if not isinstance(document, dict):
        raise CoverageContractError(f"{label} must be a JSON object: {path}")
    return document


def line_percent(entry: object, *, label: str) -> float:
    """Accept both {"line_percent": N} entries and bare numbers."""
    if isinstance(entry, dict):
        value = entry.get("line_percent")
    else:
        value = entry
    if not isinstance(value, (int, float)) or isinstance(value, bool):
        raise CoverageContractError(f"{label} has no numeric line_percent")
    return float(value)


def coverage_map(document: dict, *, label: str) -> dict[str, float]:
    files = document.get("files")
    if not isinstance(files, dict) or not files:
        raise CoverageContractError(f"{label} must contain a non-empty 'files' object")
    return {
        name: line_percent(entry, label=f"{label} file '{name}'")
        for name, entry in files.items()
    }


def overall_percent(document: dict, *, label: str) -> float:
    overall = document.get("overall")
    if overall is None:
        raise CoverageContractError(f"{label} must contain an 'overall' object")
    return line_percent(overall, label=f"{label} overall")


def evaluate(report: dict, baseline: dict, *, tolerance_pp: float | None = None) -> Verdict:
    """Pure comparison. No I/O, so the regression suite can drive it directly."""
    if tolerance_pp is None:
        raw = baseline.get("tolerance_pp", DEFAULT_TOLERANCE_PP)
        if not isinstance(raw, (int, float)) or isinstance(raw, bool) or raw < 0:
            raise CoverageContractError("baseline 'tolerance_pp' must be a non-negative number")
        tolerance_pp = float(raw)

    baseline_files = coverage_map(baseline, label="baseline")
    report_files = coverage_map(report, label="report")
    baseline_overall = overall_percent(baseline, label="baseline")
    report_overall = overall_percent(report, label="report")

    verdict = Verdict(
        tolerance_pp=tolerance_pp,
        overall_baseline=baseline_overall,
        overall_measured=report_overall,
    )

    if report_overall < baseline_overall - tolerance_pp:
        verdict.regressions.append(Regression("overall", baseline_overall, report_overall))
    elif report_overall > baseline_overall + tolerance_pp:
        verdict.improvements.append(Regression("overall", baseline_overall, report_overall))

    for name in sorted(baseline_files):
        if name not in report_files:
            verdict.missing_files.append(name)
            continue
        measured = report_files[name]
        tracked = baseline_files[name]
        if measured < tracked - tolerance_pp:
            verdict.regressions.append(Regression(name, tracked, measured))
        elif measured > tracked + tolerance_pp:
            verdict.improvements.append(Regression(name, tracked, measured))

    verdict.new_files = sorted(set(report_files) - set(baseline_files))
    return verdict


def report_verdict(verdict: Verdict, report: dict) -> int:
    scope = report.get("scope", {})
    measured_files = scope.get("measured_file_count", len(report.get("files", {})))
    print(
        f"{PREFIX} scope: native host suites only (test_build_src = false) — "
        "coverage of the firmware code the native suites compile, not whole-firmware coverage"
    )
    print(
        f"{PREFIX} measured {verdict.overall_measured:.2f}% overall across "
        f"{measured_files} file(s); baseline {verdict.overall_baseline:.2f}% "
        f"(tolerance {verdict.tolerance_pp:.2f} pp)"
    )

    if not scope.get("full_suite", True):
        print(
            f"{PREFIX} WARNING: report came from a SUBSET run — the ratchet result "
            "is not authoritative"
        )

    if verdict.missing_files:
        print(f"{PREFIX} tracked file(s) missing from the report:")
        for name in verdict.missing_files:
            print(f"  - {name}")
        print(
            f"{PREFIX} a tracked file leaving the measured scope is a coverage "
            "regression; if the file was renamed or split, refresh the baseline"
        )

    if verdict.regressions:
        print(f"{PREFIX} coverage regression contract failed:")
        for regression in verdict.regressions:
            print(f"  - {regression.describe()}")

    if verdict.failed:
        print(
            f"{PREFIX} FAIL: coverage dropped more than {verdict.tolerance_pp:.2f} pp "
            "below baseline"
        )
        return 1

    if verdict.rose:
        print(f"{PREFIX} coverage rose above baseline:")
        for improvement in verdict.improvements:
            print(f"  + {improvement.describe()}")
        for name in verdict.new_files:
            measured = line_percent(report["files"][name], label=f"report file '{name}'")
            print(f"  + {name}: {measured:.2f}% (new — not tracked in baseline)")
        print(
            f"{PREFIX} suggested baseline bump: "
            "python3 scripts/run_firmware_coverage.py --write-baseline"
        )

    print(f"{PREFIX} PASS: coverage is at or above baseline")
    return 0


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    parser.add_argument(
        "report",
        nargs="?",
        type=Path,
        default=DEFAULT_REPORT,
        help=f"Coverage report JSON (default: {DEFAULT_REPORT.relative_to(ROOT)}).",
    )
    parser.add_argument(
        "--baseline",
        type=Path,
        default=DEFAULT_BASELINE,
        help=f"Tracked baseline JSON (default: {DEFAULT_BASELINE.relative_to(ROOT)}).",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)

    try:
        baseline = load_document(args.baseline, "baseline")
        report = load_document(args.report, "coverage report")
        verdict = evaluate(report, baseline)
    except CoverageContractError as exc:
        print(f"{PREFIX} {exc}", file=sys.stderr)
        print(
            f"{PREFIX} generate a report first: python3 scripts/run_firmware_coverage.py",
            file=sys.stderr,
        )
        return 2

    return report_verdict(verdict, report)


if __name__ == "__main__":
    sys.exit(main())
