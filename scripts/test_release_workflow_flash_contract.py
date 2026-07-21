#!/usr/bin/env python3
"""Regression tests for the main-only CI-to-release trigger contract."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path
from unittest import mock

import check_release_workflow_flash_contract as contract


class MainOnlyTriggerContractTests(unittest.TestCase):
    def test_rejects_pr_only_authoritative_ci(self) -> None:
        ci_text = contract.CI_YML.read_text(encoding="utf-8")
        self.assertIn("  push:\n", ci_text)
        ci_text = ci_text.replace("  push:\n", "  pull_request:\n", 1)

        with tempfile.TemporaryDirectory(prefix="ci_trigger_contract_") as temporary:
            temporary_root = Path(temporary)
            candidate = temporary_root / ".github" / "workflows" / "ci.yml"
            candidate.parent.mkdir(parents=True)
            candidate.write_text(ci_text, encoding="utf-8")
            errors: list[str] = []
            with (
                mock.patch.object(contract, "ROOT", temporary_root),
                mock.patch.object(contract, "CI_YML", candidate),
            ):
                contract.check_production_env_is_tested(errors)

        self.assertTrue(
            any("public development branch or PR" in error for error in errors),
            errors,
        )

    def test_rejects_release_triggered_directly_by_main_push(self) -> None:
        release_text = contract.RELEASE_YML.read_text(encoding="utf-8")
        self.assertIn("  workflow_run:\n", release_text)
        release_text = release_text.replace("  workflow_run:\n", "  push:\n", 1)

        with tempfile.TemporaryDirectory(prefix="release_trigger_contract_") as temporary:
            candidate = Path(temporary) / "release.yml"
            candidate.write_text(release_text, encoding="utf-8")
            errors: list[str] = []
            with mock.patch.object(contract, "RELEASE_YML", candidate):
                contract.check_release_version_automation(errors)

        self.assertTrue(
            any("unverified main push" in error for error in errors),
            errors,
        )


if __name__ == "__main__":
    unittest.main(verbosity=2)
