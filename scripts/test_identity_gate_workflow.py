#!/usr/bin/env python3
"""Regression checks for the server-side commit-identity gate."""

from __future__ import annotations

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
WORKFLOW = ROOT / ".github" / "workflows" / "identity-gate.yml"


class IdentityGateWorkflowTests(unittest.TestCase):
    def test_gate_scans_complete_history_with_redacting_checker(self) -> None:
        text = WORKFLOW.read_text(encoding="utf-8")

        self.assertIn("push:", text)
        self.assertIn("pull_request:", text)
        self.assertIn("workflow_dispatch:", text)
        self.assertIn('tags: ["v*"]', text)
        self.assertNotIn("branches: [main]", text)
        self.assertIn("fetch-depth: 0", text)
        self.assertIn(
            "python3 scripts/check_public_commit_metadata.py --revision=--all",
            text,
        )
        self.assertIn(
            "python3 scripts/check_public_snapshot_privacy.py --all-history",
            text,
        )
        self.assertNotIn("git log", text)
        self.assertNotIn("%ae", text)
        self.assertNotIn("::error::", text)

    def test_actions_are_pinned_to_immutable_revisions(self) -> None:
        text = WORKFLOW.read_text(encoding="utf-8")
        actions = re.findall(r"uses:\s+[^@\s]+@([^\s#]+)", text)

        self.assertTrue(actions)
        for revision in actions:
            self.assertRegex(revision, r"^[0-9a-f]{40}$")


if __name__ == "__main__":
    unittest.main()
