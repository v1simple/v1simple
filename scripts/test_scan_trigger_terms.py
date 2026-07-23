#!/usr/bin/env python3
"""Regression tests for scripts/scan_trigger_terms.py.

The scanner is advisory: the load-bearing guarantees under test are (1) it detects
classifier-trigger vocabulary, (2) it never flags this repo's kept domain words,
and (3) it always exits 0 on the commit path, even when terms are present.
"""

from __future__ import annotations

import io
import sys
import tempfile
import unittest
from collections import Counter
from contextlib import redirect_stdout
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import scan_trigger_terms as sct  # noqa: E402


class ScanTextTests(unittest.TestCase):
    def test_detects_offensive_terms(self) -> None:
        counts = sct.scan_text("An attacker ran an exploit; adversarial review followed.")
        self.assertEqual(counts["attack"], 1)
        self.assertEqual(counts["exploit"], 1)
        self.assertEqual(counts["adversarial"], 1)

    def test_counts_multiple_occurrences(self) -> None:
        counts = sct.scan_text("attack attack ATTACK attacking")
        self.assertEqual(counts["attack"], 4)

    def test_kept_domain_vocab_never_flagged(self) -> None:
        # payload / inject / stealth / jammer are correct names in this repo.
        counts = sct.scan_text(
            "payload payload payload inject injection stealth stealthEnabled jammer"
        )
        self.assertEqual(sum(counts.values()), 0, f"unexpected hits: {dict(counts)}")


class DiffTests(unittest.TestCase):
    def test_added_lines_ignores_header_context_and_removals(self) -> None:
        diff = (
            "--- a/x\n"
            "+++ b/x\n"
            "@@ -0,0 +1 @@\n"
            "+an attack is added\n"
            "-a removed exploit line\n"
            " an unchanged bypass line\n"
        )
        added = sct.added_lines(diff)
        self.assertIn("attack", added)
        self.assertNotIn("exploit", added)  # removed line
        self.assertNotIn("bypass", added)   # context line


class ExitContractTests(unittest.TestCase):
    def _run_main(self, argv: list[str]) -> tuple[int, str]:
        buf = io.StringIO()
        with redirect_stdout(buf):
            rc = sct.main(argv)
        return rc, buf.getvalue()

    def test_path_mode_reports_and_exits_zero(self) -> None:
        with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as fh:
            fh.write("this text has an attacker and a backdoor and mitm\n")
            name = fh.name
        try:
            rc, out = self._run_main([name])
        finally:
            Path(name).unlink(missing_ok=True)
        self.assertEqual(rc, 0)  # advisory: present terms must NOT fail
        self.assertIn("classifier-trigger", out)

    def test_no_args_staged_mode_exits_zero(self) -> None:
        rc, out = self._run_main([])
        self.assertEqual(rc, 0)
        self.assertIn("[trigger-scan]", out)

    def test_selftest_passes(self) -> None:
        rc, _ = self._run_main(["--selftest"])
        self.assertEqual(rc, 0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
