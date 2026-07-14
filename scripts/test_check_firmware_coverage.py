#!/usr/bin/env python3
"""Offline regression tests for check_firmware_coverage.py."""

from __future__ import annotations

import copy
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Any

from check_firmware_coverage import (
    CoverageContractError,
    DEFAULT_BASELINE,
    Verdict,
    evaluate,
    main,
)

ROOT = Path(__file__).resolve().parents[1]
CHECKER = ROOT / "scripts" / "check_firmware_coverage.py"

TRACKED_A = "src/battery_manager.cpp"
TRACKED_B = "src/modules/power/power_module.cpp"
TRACKED_C = "include/battery_math.h"


def coverage_document(
    *,
    overall: float = 80.0,
    files: dict[str, float] | None = None,
    tolerance_pp: float = 0.5,
    full_suite: bool = True,
) -> dict[str, Any]:
    values = files if files is not None else {TRACKED_A: 90.0, TRACKED_B: 70.0, TRACKED_C: 100.0}
    return {
        "schema": "v1simple.firmware-coverage/1",
        "scope": {
            "environment": "native-coverage",
            "full_suite": full_suite,
            "suite_count": 3,
            "suites": ["test_battery_manager", "test_power_module", "test_settings"],
            "measured_file_count": len(values),
        },
        "tolerance_pp": tolerance_pp,
        "overall": {"line_percent": overall, "line_total": 1000, "line_covered": int(overall * 10)},
        "files": {
            name: {
                "line_percent": percent,
                "line_total": 100,
                "line_covered": int(percent),
            }
            for name, percent in values.items()
        },
    }


class EvaluateTests(unittest.TestCase):
    def test_identical_report_and_baseline_passes(self) -> None:
        baseline = coverage_document()
        verdict = evaluate(coverage_document(), baseline)
        self.assertFalse(verdict.failed)
        self.assertFalse(verdict.rose)
        self.assertEqual(verdict.regressions, [])

    def test_synthetic_overall_coverage_drop_fails(self) -> None:
        baseline = coverage_document(overall=80.0)
        # 80.00% -> 74.00%: a 6 pp collapse, far outside the 0.5 pp tolerance.
        report = coverage_document(overall=74.0)
        verdict = evaluate(report, baseline)
        self.assertTrue(verdict.failed)
        self.assertEqual([item.name for item in verdict.regressions], ["overall"])
        self.assertAlmostEqual(verdict.regressions[0].delta, -6.0)

    def test_synthetic_per_file_coverage_drop_fails_even_when_overall_holds(self) -> None:
        baseline = coverage_document()
        report = coverage_document(files={TRACKED_A: 61.0, TRACKED_B: 70.0, TRACKED_C: 100.0})
        verdict = evaluate(report, baseline)
        self.assertTrue(verdict.failed)
        self.assertEqual([item.name for item in verdict.regressions], [TRACKED_A])
        self.assertAlmostEqual(verdict.regressions[0].delta, -29.0)

    def test_drop_within_tolerance_passes(self) -> None:
        baseline = coverage_document(overall=80.0)
        report = coverage_document(
            overall=79.6,
            files={TRACKED_A: 89.6, TRACKED_B: 69.5, TRACKED_C: 100.0},
        )
        verdict = evaluate(report, baseline)
        self.assertFalse(verdict.failed)

    def test_drop_just_past_tolerance_fails(self) -> None:
        baseline = coverage_document(overall=80.0)
        report = coverage_document(overall=79.4)
        verdict = evaluate(report, baseline)
        self.assertTrue(verdict.failed)

    def test_tolerance_is_read_from_the_baseline(self) -> None:
        baseline = coverage_document(overall=80.0, tolerance_pp=5.0)
        verdict = evaluate(coverage_document(overall=76.0), baseline)
        self.assertEqual(verdict.tolerance_pp, 5.0)
        self.assertFalse(verdict.failed)

    def test_tracked_file_missing_from_report_fails(self) -> None:
        baseline = coverage_document()
        report = coverage_document(files={TRACKED_A: 90.0, TRACKED_C: 100.0})
        verdict = evaluate(report, baseline)
        self.assertTrue(verdict.failed)
        self.assertEqual(verdict.missing_files, [TRACKED_B])

    def test_rising_coverage_and_new_files_pass_and_suggest_a_bump(self) -> None:
        baseline = coverage_document(overall=80.0)
        report = coverage_document(
            overall=85.0,
            files={TRACKED_A: 96.0, TRACKED_B: 70.0, TRACKED_C: 100.0, "src/settings.cpp": 55.0},
        )
        verdict = evaluate(report, baseline)
        self.assertFalse(verdict.failed)
        self.assertTrue(verdict.rose)
        self.assertEqual([item.name for item in verdict.improvements], ["overall", TRACKED_A])
        self.assertEqual(verdict.new_files, ["src/settings.cpp"])

    def test_new_file_with_zero_coverage_does_not_fail_the_ratchet(self) -> None:
        baseline = coverage_document()
        report = coverage_document(
            files={TRACKED_A: 90.0, TRACKED_B: 70.0, TRACKED_C: 100.0, "src/brand_new.cpp": 0.0}
        )
        verdict = evaluate(report, baseline)
        self.assertFalse(verdict.failed)
        self.assertEqual(verdict.new_files, ["src/brand_new.cpp"])

    def test_bare_numeric_file_entries_are_accepted(self) -> None:
        baseline = coverage_document()
        baseline["files"] = {TRACKED_A: 90.0, TRACKED_B: 70.0, TRACKED_C: 100.0}
        baseline["overall"] = 80.0
        verdict = evaluate(coverage_document(), baseline)
        self.assertFalse(verdict.failed)

    def test_rejects_malformed_documents(self) -> None:
        cases = (
            ({"overall": {"line_percent": 80.0}}, "non-empty 'files'"),
            ({"files": {TRACKED_A: 90.0}}, "'overall'"),
            (
                {"files": {TRACKED_A: {"line_total": 5}}, "overall": {"line_percent": 80.0}},
                "numeric line_percent",
            ),
        )
        for document, expected in cases:
            with self.subTest(expected=expected):
                with self.assertRaisesRegex(CoverageContractError, expected):
                    evaluate(document, coverage_document())

    def test_rejects_negative_baseline_tolerance(self) -> None:
        baseline = coverage_document()
        baseline["tolerance_pp"] = -1.0
        with self.assertRaisesRegex(CoverageContractError, "non-negative"):
            evaluate(coverage_document(), baseline)


class CommandLineTests(unittest.TestCase):
    def run_checker(self, report: dict, baseline: dict) -> subprocess.CompletedProcess[str]:
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            report_path = directory / "coverage.json"
            baseline_path = directory / "coverage_baseline.json"
            report_path.write_text(json.dumps(report), encoding="utf-8")
            baseline_path.write_text(json.dumps(baseline), encoding="utf-8")
            return subprocess.run(
                [
                    sys.executable,
                    str(CHECKER),
                    str(report_path),
                    "--baseline",
                    str(baseline_path),
                ],
                text=True,
                capture_output=True,
                check=False,
            )

    def test_exit_zero_and_scope_disclosure_when_coverage_holds(self) -> None:
        result = self.run_checker(coverage_document(), coverage_document())
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("PASS", result.stdout)
        self.assertIn("not whole-firmware coverage", result.stdout)

    def test_exit_one_on_synthetic_coverage_drop(self) -> None:
        baseline = coverage_document(overall=80.0)
        report = coverage_document(
            overall=71.5,
            files={TRACKED_A: 42.0, TRACKED_B: 70.0, TRACKED_C: 100.0},
        )
        result = self.run_checker(report, baseline)
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("FAIL", result.stdout)
        self.assertIn("overall: 71.50% vs baseline 80.00%", result.stdout)
        self.assertIn(TRACKED_A, result.stdout)

    def test_exit_two_when_the_report_is_missing(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            baseline_path = Path(temporary) / "coverage_baseline.json"
            baseline_path.write_text(json.dumps(coverage_document()), encoding="utf-8")
            result = subprocess.run(
                [
                    sys.executable,
                    str(CHECKER),
                    str(Path(temporary) / "absent.json"),
                    "--baseline",
                    str(baseline_path),
                ],
                text=True,
                capture_output=True,
                check=False,
            )
        self.assertEqual(result.returncode, 2, result.stdout)
        self.assertIn("coverage report not found", result.stderr)

    def test_subset_reports_are_flagged_as_non_authoritative(self) -> None:
        report = coverage_document(full_suite=False)
        result = self.run_checker(report, coverage_document())
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("SUBSET run", result.stdout)

    def test_main_returns_one_for_an_in_process_synthetic_drop(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            report_path = directory / "coverage.json"
            baseline_path = directory / "baseline.json"
            report_path.write_text(json.dumps(coverage_document(overall=10.0)), encoding="utf-8")
            baseline_path.write_text(json.dumps(coverage_document()), encoding="utf-8")
            exit_code = main([str(report_path), "--baseline", str(baseline_path)])
        self.assertEqual(exit_code, 1)


class TrackedBaselineTests(unittest.TestCase):
    def test_tracked_baseline_is_well_formed_and_self_consistent(self) -> None:
        self.assertTrue(
            DEFAULT_BASELINE.exists(),
            f"tracked baseline missing: {DEFAULT_BASELINE}",
        )
        with open(DEFAULT_BASELINE, encoding="utf-8") as handle:
            baseline = json.load(handle)

        # A baseline must be a legal report: it has to pass against itself.
        verdict = evaluate(copy.deepcopy(baseline), baseline)
        self.assertIsInstance(verdict, Verdict)
        self.assertFalse(verdict.failed)
        self.assertFalse(verdict.rose)

        self.assertGreater(len(baseline["files"]), 0)
        self.assertEqual(baseline["tolerance_pp"], 0.5)
        for name in baseline["files"]:
            self.assertTrue(
                name.startswith(("src/", "include/")),
                f"baseline tracks a file outside the firmware scope: {name}",
            )

    def test_tracked_baseline_fails_a_synthetic_coverage_drop(self) -> None:
        with open(DEFAULT_BASELINE, encoding="utf-8") as handle:
            baseline = json.load(handle)

        report = copy.deepcopy(baseline)
        report["overall"]["line_percent"] = max(
            0.0, baseline["overall"]["line_percent"] - 10.0
        )
        worst = sorted(report["files"])[0]
        report["files"][worst]["line_percent"] = 0.0

        verdict = evaluate(report, baseline)
        self.assertTrue(
            verdict.failed,
            "the tracked baseline must reject a synthetic coverage collapse",
        )
        self.assertIn("overall", [item.name for item in verdict.regressions])


if __name__ == "__main__":
    unittest.main(verbosity=2)
