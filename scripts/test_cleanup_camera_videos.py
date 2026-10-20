#!/usr/bin/env python3
"""Exercise retention on disposable files; never use real bench recordings."""

import json
import os
import subprocess
import tempfile
import unittest
from datetime import datetime, timedelta, timezone
from pathlib import Path
from unittest.mock import patch

import cleanup_camera_videos as retention

NOW = datetime(2026, 10, 20, 16, tzinfo=timezone.utc)


class RetentionTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.camera = self.fixture("20261001_120000_abc123")
        probe = patch.object(retention, "verify_open_file_check")
        probe.start()
        self.addCleanup(probe.stop)

    def fixture(self, name, board="release"):
        camera = self.root / board / "runs" / name / "replay" / "camera"
        camera.mkdir(parents=True)
        for name in (".camera_preflight.mov", "evidence_exp50.mov", "evidence_exp25.mov",
                     "still.jpg", "witness.png", "report.json", "frame_timing.ndjson", "other.mov"):
            path = camera / name
            path.write_bytes(b"fixture bytes")
            old = (NOW - timedelta(days=20)).timestamp()
            os.utime(path, (old, old))
        (camera / "camera_result.json").write_text(json.dumps({"result": "CAPTURED"}))
        return camera

    def cleanup(self, apply=True):
        return retention.cleanup(self.root, apply=apply, now=NOW)

    def test_preview_changes_nothing(self):
        before = {str(path): path.read_bytes() for path in self.root.rglob("*") if path.is_file()}
        result = self.cleanup(apply=False)
        after = {str(path): path.read_bytes() for path in self.root.rglob("*") if path.is_file()}
        self.assertEqual(before, after)
        self.assertEqual(len(result["candidates"]), 3)
        self.assertEqual(result["deleted_files"], 0)

    @patch.object(retention, "videos_are_closed", return_value=True)
    def test_removes_only_old_generated_videos_and_is_idempotent(self, _closed):
        protected = {path: path.read_bytes() for path in self.camera.iterdir()
                     if not retention.VIDEO_NAME.fullmatch(path.name)}
        result = self.cleanup()
        self.assertEqual(result["deleted_files"], 3)
        self.assertEqual(protected, {path: path.read_bytes() for path in protected})
        marker = json.loads((self.camera.parent.parent / "camera_media_removed.json").read_text())
        self.assertEqual(len(marker["removed_files"]), 3)
        self.assertEqual(marker["pending_files"], [])
        self.assertEqual(self.cleanup()["deleted_files"], 0)

    @patch.object(retention, "videos_are_closed", return_value=True)
    def test_age_boundary_recent_run_and_recent_video(self, _closed):
        self.fixture("20261020_120000_abc123")
        self.fixture("20261006_160000_abc123", "boundary")
        for name in (".camera_preflight.mov", "evidence_exp50.mov", "evidence_exp25.mov"):
            timestamp = (NOW - timedelta(days=14)).timestamp()
            os.utime(self.camera / name, (timestamp, timestamp))
        self.assertEqual(self.cleanup()["deleted_files"], 0)

    @patch.object(retention, "videos_are_closed", return_value=False)
    def test_open_video_skips_entire_run(self, _closed):
        self.assertEqual(self.cleanup()["deleted_files"], 0)
        self.assertTrue((self.camera / "evidence_exp50.mov").exists())

    def test_incomplete_or_missing_result_skips_run(self):
        result = self.camera / "camera_result.json"
        result.write_text('{"result": "RECORDING"}')
        self.assertEqual(self.cleanup()["deleted_files"], 0)
        result.unlink()
        self.assertEqual(self.cleanup()["deleted_files"], 0)

    @patch.object(retention, "videos_are_closed", return_value=True)
    def test_failed_runs_and_other_boards_are_cleaned(self, _closed):
        (self.camera / "camera_result.json").write_text('{"result": "CAPTURE_FAILED"}')
        self.fixture("20261001_120000_abc123", "profile-box-enabled")
        self.assertEqual(self.cleanup()["deleted_files"], 6)

    @patch.object(retention, "videos_are_closed", return_value=True)
    def test_numbered_runs_created_on_timestamp_collision_are_cleaned(self, _closed):
        for suffix in ("2", "9", "10", "21", "100"):
            self.fixture(f"20261001_120000_abc123_{suffix}")
        self.assertEqual(self.cleanup()["deleted_files"], 18)

    @patch.object(retention, "videos_are_closed", return_value=True)
    def test_symlink_root_run_and_video_are_not_followed(self, _closed):
        link = self.root / "linked"
        link.symlink_to(self.root, target_is_directory=True)
        with self.assertRaises(ValueError):
            retention.cleanup(link, apply=True, now=NOW)
        (self.root / "release/runs/20260901_120000_abc123").symlink_to(
            self.camera.parent.parent, target_is_directory=True)
        video = self.camera / "evidence_exp50.mov"
        video.unlink()
        video.symlink_to(self.camera / "still.jpg")
        self.assertEqual(self.cleanup()["deleted_files"], 2)
        self.assertEqual((self.camera / "still.jpg").read_bytes(), b"fixture bytes")
        self.assertTrue(video.is_symlink())

    def test_replaced_video_is_skipped(self):
        def replace(_paths):
            (self.camera / "evidence_exp50.mov").write_bytes(b"new recording")
            return True
        with patch.object(retention, "videos_are_closed", side_effect=replace):
            self.assertEqual(self.cleanup()["deleted_files"], 0)

    @patch.object(retention, "videos_are_closed", return_value=True)
    def test_marker_accumulates_across_cleanup_runs(self, _closed):
        video = self.camera / "evidence_exp50.mov"
        os.utime(video, (NOW.timestamp(), NOW.timestamp()))
        self.assertEqual(self.cleanup()["deleted_files"], 2)
        later = retention.cleanup(self.root, apply=True, now=NOW + timedelta(days=15))
        self.assertEqual(later["deleted_files"], 1)
        marker = json.loads((self.camera.parent.parent / "camera_media_removed.json").read_text())
        self.assertEqual(len(marker["removed_files"]), 3)
        self.assertEqual(marker["removed_bytes"], 3 * len(b"fixture bytes"))

    @patch.object(retention, "videos_are_closed", return_value=True)
    def test_interrupted_marker_updates_keep_every_removal_accounted_for(self, _closed):
        for fail_call in (1, 2, 3, 4):
            with self.subTest(fail_call=fail_call):
                board = f"interruption-{fail_call}"
                # Isolate each run from the ordinary setUp fixture.
                root = self.root / board
                camera = self.fixture("20261001_120000_abc123", f"{board}/release")
                names = {str(path.relative_to(camera.parent.parent)) for path in camera.iterdir()
                         if retention.VIDEO_NAME.fullmatch(path.name)}
                write_marker = retention.write_marker
                calls = 0

                def interrupted_write(path, marker):
                    nonlocal calls
                    calls += 1
                    if calls == fail_call:
                        raise OSError("simulated interrupted marker update")
                    write_marker(path, marker)

                with patch.object(retention, "write_marker", side_effect=interrupted_write):
                    with self.assertRaises(OSError):
                        retention.cleanup(root, apply=True, now=NOW)
                retention.cleanup(root, apply=True, now=NOW)
                marker_path = camera.parent.parent / "camera_media_removed.json"
                marker = json.loads(marker_path.read_text())
                removed, pending = set(marker["removed_files"]), set(marker["pending_files"])
                self.assertEqual(removed | pending, names)
                self.assertFalse(removed & pending)
                self.assertEqual(len(pending), 0 if fail_call == 1 else 1)
                self.assertEqual(marker["removed_bytes"], len(removed) * len(b"fixture bytes"))
                self.assertFalse(any((camera.parent.parent / name).exists() for name in names))
                before = marker_path.read_bytes()
                self.assertEqual(retention.cleanup(root, apply=True, now=NOW)["deleted_files"], 0)
                self.assertEqual(marker_path.read_bytes(), before)

    @patch.object(retention, "videos_are_closed", return_value=True)
    def test_invalid_marker_cannot_delete_a_video(self, _closed):
        marker = self.camera.parent.parent / "camera_media_removed.json"
        for value in ([], {"removed_files": "bad", "removed_bytes": 0},
                      {"removed_files": [], "removed_bytes": "bad"},
                      {"removed_files": [], "removed_bytes": 0, "pending_files": "bad"},
                      {"removed_files": [], "removed_bytes": 0, "pending_files": [3]}):
            marker.write_text(json.dumps(value))
            with self.assertRaises(ValueError):
                self.cleanup()
            self.assertTrue((self.camera / "evidence_exp50.mov").exists())

    @patch.object(retention.shutil, "which", return_value="/usr/sbin/lsof")
    def test_lsof_failures_never_allow_deletion(self, _which):
        for code, stdout, stderr in ((0, "nopen.mov", ""), (1, "", "permission denied"), (2, "", "")):
            with patch.object(retention.subprocess, "run",
                              return_value=subprocess.CompletedProcess([], code, stdout, stderr)):
                if code == 0:
                    self.assertFalse(retention.videos_are_closed([self.camera / "evidence_exp50.mov"]))
                else:
                    with self.assertRaises(RuntimeError):
                        self.cleanup()
                self.assertTrue((self.camera / "evidence_exp50.mov").exists())

    @patch.object(retention.shutil, "which", return_value=None)
    def test_missing_lsof_cannot_delete_a_video(self, _which):
        with self.assertRaises(RuntimeError):
            self.cleanup()
        self.assertTrue((self.camera / "evidence_exp50.mov").exists())


class OpenFileProbeTests(unittest.TestCase):
    @patch.object(retention, "videos_are_closed", return_value=True)
    def test_silent_process_restriction_fails_closed(self, _closed):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(RuntimeError):
                retention.verify_open_file_check(Path(directory).resolve())
            self.assertEqual(list(Path(directory).iterdir()), [])


if __name__ == "__main__":
    unittest.main()
