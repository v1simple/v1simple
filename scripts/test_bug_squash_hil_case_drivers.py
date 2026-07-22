#!/usr/bin/env python3
"""Regression tests for the tracked bug-squash HIL case-driver registry."""

from __future__ import annotations

from dataclasses import replace
import json
from pathlib import Path
import subprocess
import unittest
from unittest import mock

import bug_squash_hil_case_drivers as case_drivers
import check_bug_squash_hil_qualification as qualification
import run_bug_squash_hil as runner


ROOT = Path(__file__).resolve().parents[1]


class CaseDriverRegistryTests(unittest.TestCase):
    def test_registry_covers_each_pinned_case_exactly_once(self) -> None:
        profile, errors = qualification.load_pinned_profile()
        self.assertEqual(errors, [])
        assert profile is not None
        profile_case_ids = tuple(case["id"] for case in profile["required_cases"])
        self.assertEqual(case_drivers.CASE_IDS, profile_case_ids)
        self.assertEqual(tuple(case_drivers.DRIVER_BY_CASE), profile_case_ids)
        self.assertEqual(len(case_drivers.DRIVERS), len(set(case_drivers.DRIVER_BY_CASE)))

    def test_profile_binds_registry_and_runner_sources(self) -> None:
        profile, errors = qualification.load_pinned_profile()
        self.assertEqual(errors, [])
        assert profile is not None
        contract = profile["case_driver_contract"]
        self.assertEqual(contract["status"], "active")
        self.assertEqual(contract["runner_path"], case_drivers.RUNNER_SOURCE_PATH)
        self.assertEqual(
            contract["driver_source_paths"],
            [case_drivers.REGISTRY_SOURCE_PATH],
        )
        with mock.patch.object(
            qualification,
            "git_blob_sha256",
            return_value="a" * 64,
        ):
            provenance = qualification.expected_execution_provenance(profile, "b" * 40)
        tracked = {source["path"] for source in provenance["tracked_sources"]}
        self.assertIn(case_drivers.RUNNER_SOURCE_PATH, tracked)
        self.assertIn(case_drivers.REGISTRY_SOURCE_PATH, tracked)

    def test_implemented_dispatch_contract_is_exact(self) -> None:
        handlers = runner.case_handler_map()
        self.assertEqual(tuple(handlers), case_drivers.implemented_entrypoints())
        for driver in case_drivers.DRIVERS:
            if driver.implemented:
                self.assertIs(runner.resolve_case_handler(driver), handlers[driver.entrypoint])

    def test_blocked_drivers_fail_before_handler_resolution(self) -> None:
        blocked = [driver for driver in case_drivers.DRIVERS if not driver.implemented]
        self.assertEqual(len(blocked), 9)
        with mock.patch.object(
            runner,
            "case_handler_map",
            side_effect=AssertionError("handler lookup crossed the unavailable boundary"),
        ):
            for driver in blocked:
                with self.subTest(case_id=driver.case_id), self.assertRaises(
                    runner.CaseDriverUnavailable
                ):
                    runner.resolve_case_handler(driver)

    def test_substituted_entrypoint_fails_before_handler_execution(self) -> None:
        original = case_drivers.get_case_driver("BSC-02")
        substituted = replace(original, entrypoint="run_bsc05_case")
        called = False

        def unexpected_handler(_args: object) -> int:
            nonlocal called
            called = True
            return 0

        handlers = {
            entrypoint: unexpected_handler
            for entrypoint in case_drivers.implemented_entrypoints()
        }
        with mock.patch.object(
            runner,
            "case_handler_map",
            return_value=handlers,
        ), self.assertRaises(runner.RunnerError) as raised:
            runner.resolve_case_handler(substituted)
        self.assertEqual(raised.exception.code, "case_driver_contract_invalid")
        self.assertFalse(called)

    def test_unavailable_cli_cases_return_the_pinned_error(self) -> None:
        for driver in case_drivers.DRIVERS:
            if driver.implemented:
                continue
            with self.subTest(case_id=driver.case_id):
                completed = subprocess.run(
                    [
                        "python3",
                        "-B",
                        str(ROOT / "scripts" / "run_bug_squash_hil.py"),
                        "--case",
                        driver.case_id,
                        "--board",
                        "opaque-dut",
                    ],
                    cwd=ROOT,
                    capture_output=True,
                    text=True,
                    check=False,
                )
                self.assertEqual(completed.returncode, 3, completed.stderr)
                payload = json.loads(completed.stdout)
                self.assertEqual(payload["error"]["code"], "case_driver_unavailable")

    def test_bsc11_owns_its_blocker_contract(self) -> None:
        bsc03 = case_drivers.get_case_driver("BSC-03")
        bsc11 = case_drivers.get_case_driver("BSC-11")
        self.assertIn("hil-fault-control-not-implemented", bsc03.qualification_blockers)
        self.assertNotIn("hil-fault-control-not-implemented", bsc11.qualification_blockers)
        self.assertIn("tracked-rig-adapter-not-implemented", bsc11.qualification_blockers)

    def test_bsc16_owns_the_implemented_fault_hook_contract(self) -> None:
        bsc16 = case_drivers.get_case_driver("BSC-16")
        self.assertTrue(bsc16.implemented)
        self.assertNotIn("hil-fault-control-not-implemented", bsc16.qualification_blockers)
        self.assertIn("tracked-rig-adapter-not-implemented", bsc16.qualification_blockers)
        self.assertIs(runner.resolve_case_handler(bsc16), runner.run_bsc16_case)

    def test_bsc04_owns_the_implemented_fault_hook_contract(self) -> None:
        bsc04 = case_drivers.get_case_driver("BSC-04")
        self.assertTrue(bsc04.implemented)
        self.assertNotIn("hil-fault-control-not-implemented", bsc04.qualification_blockers)
        self.assertIn("tracked-rig-adapter-not-implemented", bsc04.qualification_blockers)
        self.assertIs(runner.resolve_case_handler(bsc04), runner.run_bsc04_case)

    def test_authoritative_ci_runs_the_declared_registry_gate(self) -> None:
        ci_source = (ROOT / "scripts" / "ci-test.sh").read_text(encoding="utf-8")
        self.assertIn("python3 scripts/test_bug_squash_hil_case_drivers.py", ci_source)


if __name__ == "__main__":
    unittest.main()
