#!/usr/bin/env python3
"""Recorded revision, source drift and repair guidance for physical findings."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent))
from bench import encounter_behavior_contract as contract


ROOT = Path(__file__).resolve().parents[1]


def git(root, *args):
    return subprocess.run(["git", "-C", str(root), *args], capture_output=True,
                          check=True).stdout.decode().strip()


class BehaviorContractTests(unittest.TestCase):
    def test_recorded_source_is_resolved_not_relabelled_as_tooling_head(self):
        result = contract.behavior_contract(ROOT, "ee6b401")
        self.assertEqual(result["status"], "VERIFIED")
        recorded = git(ROOT, "rev-parse", "ee6b401^{commit}")
        self.assertEqual(result["firmware_commit"], recorded)
        self.assertIsNone(result["response_deadline_ms"])
        self.assertEqual(len(result["field_rule_ids"]), 7)
        rule = result["rules"]["retired_card"]
        self.assertIn("subsequent card render", rule["statement"])
        self.assertIn("does not prove", rule["repair_direction"])
        for location in rule["locations"]:
            self.assertIn(f"/blob/{recorded}/", location["url"])
            self.assertTrue(location["excerpt"])
        self.assertIn("gracePeriodMs = 1", rule["locations"][2]["excerpt"])
        idle = result["rules"]["idle_volume_warning"]
        self.assertIn("0x1082", idle["statement"])
        self.assertIn("brightness threshold cannot establish", idle["repair_direction"])
        self.assertTrue(any("drawFrequency(0, BAND_NONE" in location["excerpt"]
                            for location in idle["locations"]))
        self.assertTrue(any(".colorGray = 0x1082" in location["excerpt"]
                            for location in idle["locations"]))

    def test_same_rules_across_distinct_recorded_firmware_builds(self):
        older = contract.behavior_contract(ROOT, "d50ae47")
        newer = contract.behavior_contract(ROOT, "ee6b401")
        self.assertEqual(older["status"], "VERIFIED")
        self.assertNotEqual(older["firmware_commit"], newer["firmware_commit"])
        self.assertEqual(older["comparison_key"], newer["comparison_key"])
        for path in older["sources"]:
            self.assertEqual(older["sources"][path]["sha256"], newer["sources"][path]["sha256"])

    def test_zero_persistence_repair_has_its_own_reviewed_explanation(self):
        before = contract.behavior_contract(ROOT, "d67bad1")
        after = contract.behavior_contract(ROOT, "9dba7dc")
        self.assertEqual(before["status"], "VERIFIED")
        self.assertEqual(after["status"], "VERIFIED")
        self.assertEqual(before["comparison_key"], after["comparison_key"])
        self.assertIn("Zero is converted to 1 ms", before["rules"]["retired_card"]["statement"])
        rule = after["rules"]["retired_card"]
        self.assertIn("released on the current card render", rule["statement"])
        self.assertIn("only when persistence is positive", rule["statement"])
        self.assertIn("already corrected", rule["repair_direction"])
        self.assertIn("gracePeriodMs == 0", rule["locations"][0]["excerpt"])
        self.assertIn("gracePeriodMs > 0", rule["locations"][1]["excerpt"])
        self.assertNotIn("gracePeriodMs = 1", "\n".join(loc["excerpt"] for loc in rule["locations"]))
        self.assertEqual(after["sources"]["src/display_cards.cpp"]["sha256"], contract._FIXED_CARD_SOURCE)
        for name in ("secondary", "physical_dispatch"):
            self.assertEqual(before["rules"][name]["statement"], after["rules"][name]["statement"])
        self.assertIn("FILL_RECT", after["rules"]["physical_dispatch"]["locations"][0]["excerpt"])

    def test_further_card_source_change_does_not_inherit_repair_explanation(self):
        real_git = contract._git

        def changed(root, *args):
            data = real_git(root, *args)
            if args[0] == "show" and args[1].endswith(":src/display_cards.cpp"):
                return data + b"\n// unreviewed change\n"
            return data

        with patch.object(contract, "_git", side_effect=changed):
            result = contract.behavior_contract(ROOT, "9dba7dc")
        self.assertEqual(result["status"], "UNREVIEWED")
        for name in ("secondary", "retired_card", "physical_dispatch"):
            self.assertEqual(result["rules"][name]["status"], "UNREVIEWED")
            self.assertNotIn("already corrected", result["rules"][name]["repair_direction"])
            self.assertTrue(all(loc["excerpt"] is None for loc in result["rules"][name]["locations"]))

    def test_invalid_revision_never_reaches_git(self):
        with patch.object(contract, "_git") as call:
            for revision in (None, "HEAD", "--help", "abc", "ee6b401:path", "$(bad)"):
                self.assertEqual(contract.behavior_contract(ROOT, revision)["status"], "SOURCE_UNAVAILABLE")
            call.assert_not_called()

    def test_missing_recorded_revision_does_not_fall_back_to_head(self):
        result = contract.behavior_contract(ROOT, "0" * 40)
        self.assertEqual(result["status"], "SOURCE_UNAVAILABLE")
        self.assertIsNone(result["firmware_commit"])
        self.assertFalse(result["rules"])

    def test_changed_source_withholds_only_affected_rule_and_ignores_dirty_checkout(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            git(root, "init", "-q")
            git(root, "config", "user.name", "bench-test")
            git(root, "config", "user.email", "noreply@example.invalid")
            for path in contract._SOURCES:
                target = root / path
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(subprocess.run(
                    ["git", "-C", str(ROOT), "show", "ee6b401:" + path],
                    capture_output=True, check=True).stdout)
            git(root, "add", ".")
            git(root, "commit", "-qm", "reviewed fixture")
            original = git(root, "rev-parse", "HEAD")
            frequency = root / "src/display_frequency.cpp"
            frequency.write_text(frequency.read_text().replace('"%05.3f"', '"%05.2f"'))
            # Uncommitted tooling/working-tree edits cannot rewrite recorded source.
            self.assertEqual(contract.behavior_contract(root, original)["status"], "VERIFIED")
            git(root, "add", "src/display_frequency.cpp")
            git(root, "commit", "-qm", "different frequency formatting fixture")
            changed = git(root, "rev-parse", "HEAD")
            result = contract.behavior_contract(root, changed)
            self.assertEqual(result["status"], "UNREVIEWED")
            self.assertEqual(result["rules"]["primary_frequency"]["status"], "UNREVIEWED")
            self.assertEqual(result["rules"]["main_bars"]["status"], "VERIFIED")
            self.assertIsNone(result["rules"]["primary_frequency"]["locations"][0]["excerpt"])
            self.assertNotIn("three decimal", result["rules"]["primary_frequency"]["statement"])
            self.assertEqual(result["comparison_key"], contract.behavior_contract(root, original)["comparison_key"])
            git(root, "rm", "-q", "src/display_arrow.cpp")
            git(root, "commit", "-qm", "removed arrow owner fixture")
            missing = contract.behavior_contract(root, git(root, "rev-parse", "HEAD"))
            self.assertEqual(missing["sources"]["src/display_arrow.cpp"]["status"], "UNAVAILABLE")
            self.assertEqual(missing["rules"]["main_arrows"]["status"], "UNREVIEWED")


if __name__ == "__main__":
    unittest.main()
