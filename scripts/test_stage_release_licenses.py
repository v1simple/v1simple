#!/usr/bin/env python3
"""Regression tests for release-license staging."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

import stage_release_licenses as licenses


class ReleaseLicenseStagingTests(unittest.TestCase):
    def test_live_sources_are_present_and_pinned_sources_are_exact(self) -> None:
        self.assertEqual(licenses.source_errors(licenses.ROOT), [])

    def test_stage_copies_exact_files_into_both_release_packages(self) -> None:
        with tempfile.TemporaryDirectory(prefix="release_licenses_") as temporary:
            root = Path(temporary)
            release = root / "release"
            pages = release / "pages"
            release.mkdir()
            pages.mkdir()
            (release / "manifest.json").write_text("{}\n", encoding="utf-8")
            (pages / "index.html").write_text("installer\n", encoding="utf-8")

            licenses.stage(licenses.ROOT, release, pages)

            self.assertEqual(licenses.staged_errors(licenses.ROOT, release, pages), [])
            self.assertEqual((release / "manifest.json").read_text(), "{}\n")
            self.assertEqual((pages / "index.html").read_text(), "installer\n")
            for source, destination in licenses.staged_paths(
                licenses.ROOT, release, pages
            ):
                self.assertEqual(destination.read_bytes(), source.read_bytes())

    def test_missing_source_stops_staging(self) -> None:
        with tempfile.TemporaryDirectory(prefix="release_license_source_") as temporary:
            root = Path(temporary)
            for relative in (*licenses.ROOT_FILES, *licenses.LICENSE_FILES):
                source = root / relative
                source.parent.mkdir(parents=True, exist_ok=True)
                source.write_bytes(b"license\n")
            (root / licenses.LICENSE_FILES[0]).unlink()

            with self.assertRaisesRegex(ValueError, "missing release license source"):
                licenses.stage(root, root / "release", root / "pages")

    def test_corrupt_staged_file_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory(prefix="release_license_output_") as temporary:
            root = Path(temporary)
            release = root / "release"
            pages = root / "pages"
            licenses.stage(licenses.ROOT, release, pages)
            (release / "LICENSE").write_text("changed\n", encoding="utf-8")

            errors = licenses.staged_errors(licenses.ROOT, release, pages)

            self.assertTrue(any("differs from source" in error for error in errors), errors)


if __name__ == "__main__":
    unittest.main(verbosity=2)
