"""Focused reader checks; the retained recordings supply the real negative control."""

import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

import numpy as np
from PIL import Image

from bench import read_replay_fields as reader
from bench.camera_artifacts import build_capture_manifest
from bench.read_replay_fields import CARD_TEMPLATES, read_pixels, secondary_identities


class ReadReplayFieldsTest(unittest.TestCase):
    @staticmethod
    def radar_frame():
        image = np.zeros((720, 1280, 3), dtype=np.uint8)
        orange = (240, 100, 40)

        def digit(character, x, y):
            # Independent segment drawing: top, middle, bottom, then the sides.
            segments = {"1": (4, 6), "2": (0, 1, 2, 4, 5),
                        "3": (0, 1, 2, 4, 6), "4": (1, 3, 4, 6),
                        "5": (0, 1, 2, 3, 6), "0": (0, 2, 3, 4, 5, 6)}
            rectangles = ((0, 8, 0, 40), (32, 40, 0, 40), (64, 72, 0, 40),
                          (8, 32, 0, 8), (8, 32, 32, 40),
                          (40, 64, 0, 8), (40, 64, 32, 40))
            for index in segments[character]:
                y0, y1, x0, x1 = rectangles[index]
                image[y + y0:y + y1, x + x0:x + x1] = orange

        for character, x in zip("24150", (465, 520, 600, 660, 715)):
            digit(character, x, 280)
        image[342:352, 580:590] = orange
        digit("3", 250, 195)
        image[335:375, 335:390] = (240, 240, 240)  # K field.
        image[210:285, 1030:1150] = orange  # FRONT field.
        for index, character in enumerate("MRCT"):
            x = 460 + 19 * index
            image[385:409, x:x + 16][CARD_TEMPLATES[character]] = (200, 40, 170)
        image[393:401, 424:444] = (240, 240, 240)
        return image

    @staticmethod
    def camera_pose(image, scale=1., dx=0, dy=0):
        moved = np.asarray(Image.fromarray(image).transform(
            (1280, 720), Image.Transform.AFFINE,
            (1 / scale, 0, -dx / scale, 0, 1 / scale, -dy / scale),
            resample=Image.Resampling.BILINEAR))
        x, y, width, height = reader.READER_REFERENCE_CROP
        registration = {"result": "PASS", "transform": {
            "kind": "dynamic_similarity",
            "crop_fractions": [scale * x + dx / 1280, scale * y + dy / 720,
                               scale * width, scale * height]}}
        return moved, registration

    def make_run(self, run, registration, *, declared=False):
        camera = run / "camera"
        camera.mkdir()
        (run / "window_result.json").write_text(json.dumps({
            "result": "COMPLETE", "runtime_qualification": {"status": "qualified"},
            "camera": {"result": "CAPTURED", "capture_id": "fixture",
                       "video_timing_verification_result": {"status": "verified"}},
            "git_sha": "fixture", "runtime_identity": {"image_id": "fixture"},
            "artifacts": {}}))
        expected = {"alerts": [
            {"priority": True, "band": "k", "frequencyMHz": 24150, "direction": "FRONT"},
            {"priority": False, "band": "k", "frequencyMHz": 24125,
             "direction": "SIDE", "photoType": 1}],
            "muted": False, "bogeyCounterChar": "3", "bandBlink": False, "arrowBlink": False}
        (run / "replay_stimulus.ndjson").write_text("".join(json.dumps({
            "stimulusSequence": index, "requestedHostMonotonicNs": 1_000_000_000,
            "expected": expected}) + "\n" for index in range(1, 4)))
        (camera / "frame_timing.ndjson").write_text("".join(json.dumps({
            "status": "written", "host_capture_ns": when,
            "video_pts_value": when, "video_pts_timescale": 1_000_000_000}) + "\n"
            for when in (1_000_000_000, 1_180_000_000)))
        (camera / "camera_preflight.json").write_text(json.dumps({
            "kind": "bench_camera_preflight", "result": "PASS", "registration": registration}))
        (camera / "evidence_fixture.mov").touch()
        if declared:
            result = {"result": "CAPTURED", "video": "evidence_fixture.mov",
                      "frame_timing": "frame_timing.ndjson",
                      "video_timing_verification_result": {"status": "verified"}}
            (camera / result["video"]).write_bytes(b"fixture video")
            for field in ("video_timing_verification", "session_start_still", "bright_still", "dim_still"):
                result[field] = field + ".fixture"
                (camera / result[field]).write_bytes(b"fixture")
            manifest = build_capture_manifest(camera_dir=camera, camera_result=result, suite="replay")
            self.publish_manifest(run, manifest)
        self.publish_display_contract(run)
        return run

    @staticmethod
    def publish_manifest(run, manifest):
        # Re-sign explicitly changed fixture ownership, just as the producer does.
        identity = json.dumps(manifest["identity"], ensure_ascii=True, allow_nan=False,
                              sort_keys=True, separators=(",", ":")).encode()
        manifest["capture_id"] = hashlib.sha256(identity).hexdigest()
        (run / "camera/capture_manifest.json").write_text(json.dumps(manifest))
        window_path = run / "window_result.json"
        window = json.loads(window_path.read_text())
        window["camera"].update(capture_manifest="capture_manifest.json", capture_id=manifest["capture_id"])
        window_path.write_text(json.dumps(window))
        ReadReplayFieldsTest.publish_display_contract(run)

    @staticmethod
    def publish_display_contract(run, stimulus_name="replay_stimulus.ndjson", *, schema=2):
        def entry(path):
            data = path.read_bytes()
            return {"path": path.name, "size_bytes": len(data),
                    "sha256": hashlib.sha256(data).hexdigest()}
        window_path = run / "window_result.json"
        window = json.loads(window_path.read_text())
        contract = {"schema_version": 1, "kind": "bench_replay_display_contract",
                    "expected_display_rubric": entry(run / stimulus_name)}
        if "capture_manifest" in window["camera"]:
            manifest = run / "camera" / window["camera"]["capture_manifest"]
            build = run / "build_upload_artifacts.json"
            build.write_text(json.dumps({"expected_runtime_image_id": "b" * 9}))
            window["artifacts"]["build_upload"] = entry(build)
            window["git_sha"] = "a" * 40
            window["runtime_identity"] = {"boot_id": 3, "git_sha": "a" * 7, "image_id": "b" * 9}
            runtime = {**window["runtime_identity"], "build_upload_manifest_sha256": entry(build)["sha256"]}
            if schema == 2:
                runtime["source_git_sha"] = window["git_sha"]
            contract.update(schema_version=schema, runtime=runtime, camera={
                "capture_id": window["camera"]["capture_id"],
                "manifest": {**entry(manifest), "path": f"camera/{manifest.name}"}})
        path = run / "replay_display_contract.json"
        path.write_text(json.dumps(contract))
        window["artifacts"]["replay_display_contract"] = entry(path)
        window_path.write_text(json.dumps(window))

    def test_replay_contract_and_stimulus_changes_cannot_receive_a_visual_pass(self):
        for name in ("replay_display_contract.json", "replay_stimulus.ndjson"):
            for missing in (False, True):
                with self.subTest(name=name, missing=missing), tempfile.TemporaryDirectory() as temporary:
                    image, registration = self.camera_pose(self.radar_frame())
                    run = self.make_run(Path(temporary), registration, declared=True)
                    path = run / name
                    if missing:
                        path.unlink()
                    elif name.endswith(".ndjson"):
                        path.write_bytes(path.read_bytes().replace(b"24150", b"24151"))
                    else:
                        path.write_bytes(path.read_bytes().replace(b'"schema_version": 2', b'"schema_version": 9'))
                    with (mock.patch.object(reader, "frame_at", return_value=(1.18, image)) as decode,
                          self.assertRaisesRegex(ValueError, "replay artifact changed|disappeared")):
                        reader.compare_run(run)
                    decode.assert_not_called()

    def test_replay_reader_uses_verified_stimulus_path_and_rejects_missing_contract(self):
        with tempfile.TemporaryDirectory() as temporary:
            image, registration = self.camera_pose(self.radar_frame())
            run = self.make_run(Path(temporary), registration, declared=True)
            original = run / "replay_stimulus.ndjson"
            original.rename(run / "retained_stimulus.ndjson")
            original.write_bytes(b"unverified stale default")
            self.publish_display_contract(run, "retained_stimulus.ndjson")
            with mock.patch.object(reader, "frame_at", return_value=(1.18, image)):
                report = reader.compare_run(run)
            self.assertEqual("PASS", report["result"])
            window_path = run / "window_result.json"
            window = json.loads(window_path.read_text())
            del window["artifacts"]["replay_display_contract"]
            window_path.write_text(json.dumps(window))
            with (mock.patch.object(reader, "frame_at", return_value=(1.18, image)) as decode,
                  self.assertRaisesRegex(ValueError, "replay artifact ownership")):
                reader.compare_run(run)
            decode.assert_not_called()

    def compare_frame(self, image, registration):
        with tempfile.TemporaryDirectory() as temporary:
            run = self.make_run(Path(temporary), registration)
            with mock.patch.object(reader, "frame_at", return_value=(1.18, image)):
                return reader.compare_run(run)

    def test_manifest_integrity_preserves_valid_and_wrong_visible_fields(self):
        for wrong_band in (False, True):
            with self.subTest(wrong_band=wrong_band), tempfile.TemporaryDirectory() as temporary:
                image, registration = self.camera_pose(self.radar_frame())
                if wrong_band:
                    image = image.copy()
                    image[335:375, 335:390] = 0
                    image[405:445, 335:390] = (240, 240, 240)
                run = self.make_run(Path(temporary), registration, declared=True)
                with mock.patch.object(reader, "frame_at", return_value=(1.18, image)):
                    report = reader.compare_run(run)
                self.assertEqual("FAIL" if wrong_band else "PASS", report["result"])
                self.assertEqual("verified", report["source"]["cameraIntegrity"])
                self.assertEqual("verified", report["source"]["runtimeIntegrity"])
                self.assertEqual("a" * 40, report["source"]["gitSha"])
                self.assertEqual("b" * 9, report["source"]["imageId"])
                self.assertEqual(3, report["source"]["bootId"])
        image, registration = self.camera_pose(self.radar_frame())
        report = self.compare_frame(image, registration)
        self.assertEqual("PASS", report["result"])
        self.assertEqual("legacy-unverified", report["source"]["cameraIntegrity"])
        self.assertEqual("legacy-unverified", report["source"]["runtimeIntegrity"])
        self.assertIsNone(report["source"]["gitSha"])

    def test_legacy_bound_contract_preserves_verified_firmware_abbreviation(self):
        with tempfile.TemporaryDirectory() as temporary:
            image, registration = self.camera_pose(self.radar_frame())
            run = self.make_run(Path(temporary), registration, declared=True)
            self.publish_display_contract(run, schema=1)
            with mock.patch.object(reader, "frame_at", return_value=(1.18, image)):
                report = reader.compare_run(run)
            self.assertEqual("PASS", report["result"])
            self.assertEqual("a" * 7, report["source"]["gitSha"])
            self.assertEqual("verified", report["source"]["runtimeIntegrity"])

    def test_current_display_contract_producer_matches_visual_reader(self):
        from test_bench_window import run_window_module as producer

        with tempfile.TemporaryDirectory() as temporary:
            image, registration = self.camera_pose(self.radar_frame())
            run = self.make_run(Path(temporary), registration, declared=True)
            window_path = run / "window_result.json"
            window = json.loads(window_path.read_text())
            artifacts = window["artifacts"]
            rubric_path = run / "replay_stimulus.ndjson"
            events = [json.loads(line) for line in rubric_path.read_text().splitlines()]
            rubric_path.write_text("".join(json.dumps({**event, "schemaVersion": 4,
                "state": "stimulus_requested", "notifications": []}) + "\n" for event in events))
            artifacts["replay_stimulus"] = {**producer.file_artifact(rubric_path), "event_count": len(events)}
            for name in ("presentation_snapshot", "replay_delivery", "replay_scenario",
                         "v1_emulator_state", "bench_timeline"):
                path = run / f"{name}.json"
                path.write_text('{"schemaVersion": 2, "samples": []}')
                artifacts[name] = producer.file_artifact(path)
            (run / "replay_display_contract.json").unlink()
            artifacts["replay_display_contract"] = producer.publish_replay_display_contract(
                run, artifacts=artifacts, runtime_identity=window["runtime_identity"],
                source_commit=window["git_sha"], camera_result=window["camera"])
            window_path.write_text(json.dumps(window))
            with mock.patch.object(reader, "frame_at", return_value=(1.18, image)):
                report = reader.compare_run(run)
            self.assertEqual("PASS", report["result"])
            self.assertEqual(window["git_sha"], report["source"]["gitSha"])
            self.assertEqual(window["camera"]["capture_id"], report["source"]["cameraCaptureId"])

    def test_window_cannot_reassign_bound_firmware_or_camera(self):
        faults = {
            "boot": lambda w: w["runtime_identity"].update(boot_id=4),
            "image": lambda w: w["runtime_identity"].update(image_id="c" * 9),
            "full_commit_same_prefix": lambda w: w.update(git_sha="a" * 7 + "c" * 33),
            "both_git_fields": lambda w: (w.update(git_sha="c" * 40),
                                          w["runtime_identity"].update(git_sha="c" * 7)),
            "build_receipt": lambda w: w["artifacts"].pop("build_upload"),
            "missing_camera": lambda w: w["camera"].pop("capture_manifest"),
        }
        for fault, change in faults.items():
            with self.subTest(fault=fault), tempfile.TemporaryDirectory() as temporary:
                image, registration = self.camera_pose(self.radar_frame())
                run = self.make_run(Path(temporary), registration, declared=True)
                window_path = run / "window_result.json"
                window = json.loads(window_path.read_text())
                change(window)
                window_path.write_text(json.dumps(window))
                with (mock.patch.object(reader, "frame_at", return_value=(1.18, image)) as decode,
                      self.assertRaisesRegex((ValueError, RuntimeError), "display contract")):
                    reader.compare_run(run)
                decode.assert_not_called()

    def test_declared_capture_rejects_corrupt_or_missing_files_before_decoding(self):
        for artifact in ("frame_timing", "video", "preflight", "bright_still"):
            for missing in (False, True):
                with self.subTest(artifact=artifact, missing=missing), tempfile.TemporaryDirectory() as temporary:
                    image, registration = self.camera_pose(self.radar_frame())
                    run = self.make_run(Path(temporary), registration, declared=True)
                    manifest = json.loads((run / "camera/capture_manifest.json").read_text())
                    path = run / "camera" / manifest["identity"]["artifacts"][artifact]["path"]
                    if missing:
                        path.unlink()
                    else:
                        original = path.read_bytes()
                        if artifact == "frame_timing":
                            changed = original.replace(b"1180000000", b"1180000001")
                        else:
                            changed = bytes([original[0] ^ 1]) + original[1:]
                        self.assertNotEqual(original, changed)
                        self.assertEqual(len(original), len(changed))
                        path.write_bytes(changed)
                    with (mock.patch.object(reader, "frame_at", return_value=(1.18, image)) as decode,
                          self.assertRaisesRegex(RuntimeError, "changed|disappeared")):
                        reader.compare_run(run)
                    decode.assert_not_called()

    def test_declared_manifest_cannot_silently_become_legacy(self):
        for declaration in (None, "", False, [], "../capture_manifest.json", "missing.json"):
            with self.subTest(declaration=declaration), tempfile.TemporaryDirectory() as temporary:
                image, registration = self.camera_pose(self.radar_frame())
                run = self.make_run(Path(temporary), registration, declared=True)
                path = run / "window_result.json"
                window = json.loads(path.read_text())
                window["camera"]["capture_manifest"] = declaration
                path.write_text(json.dumps(window))
                with (mock.patch.object(reader, "frame_at", return_value=(1.18, image)) as decode,
                      self.assertRaisesRegex(RuntimeError, "manifest|artifact")):
                    reader.compare_run(run)
                decode.assert_not_called()

    def test_manifest_identity_schema_and_replay_suite_are_required(self):
        for change in ("window-id", "identity-digest", "schema", "suite", "suite-shape", "manifest-summary"):
            with self.subTest(change=change), tempfile.TemporaryDirectory() as temporary:
                image, registration = self.camera_pose(self.radar_frame())
                run = self.make_run(Path(temporary), registration, declared=True)
                manifest_path = run / "camera/capture_manifest.json"
                manifest = json.loads(manifest_path.read_text())
                if change == "window-id":
                    window_path = run / "window_result.json"
                    window = json.loads(window_path.read_text())
                    window["camera"]["capture_id"] = "mismatched"
                    window_path.write_text(json.dumps(window))
                elif change in {"suite", "suite-shape"}:
                    manifest["identity"]["suite"] = "display" if change == "suite" else []
                    self.publish_manifest(run, manifest)
                elif change == "manifest-summary":
                    manifest["timestamp_utc"] = "altered fixture timestamp"
                    manifest_path.write_text(json.dumps(manifest))
                else:
                    manifest["schema_version" if change == "schema" else "capture_id"] = "invalid"
                    manifest_path.write_text(json.dumps(manifest))
                with (mock.patch.object(reader, "frame_at", return_value=(1.18, image)) as decode,
                      self.assertRaisesRegex((ValueError, RuntimeError), "schema|identity|suite|artifact changed")):
                    reader.compare_run(run)
                decode.assert_not_called()

    def test_reader_consumes_verified_manifest_paths_instead_of_default_paths(self):
        with tempfile.TemporaryDirectory() as temporary:
            image, registration = self.camera_pose(self.radar_frame())
            run = self.make_run(Path(temporary), registration, declared=True)
            camera = run / "camera"
            manifest = json.loads((camera / "capture_manifest.json").read_text())
            for artifact in ("video", "frame_timing", "preflight"):
                entry = manifest["identity"]["artifacts"][artifact]
                original = camera / entry["path"]
                entry["path"] = "verified_" + entry["path"]
                original.rename(camera / entry["path"])
                original.write_bytes(b"unverified stale default file")
            self.publish_manifest(run, manifest)
            with mock.patch.object(reader, "frame_at", return_value=(1.18, image)) as decode:
                report = reader.compare_run(run)
            self.assertEqual("PASS", report["result"])
            self.assertEqual("verified", report["source"]["cameraIntegrity"])
            self.assertEqual(camera / manifest["identity"]["artifacts"]["video"]["path"], decode.call_args.args[0])

    def test_integrity_errors_are_inconclusive_in_script_and_package_commands(self):
        scripts = Path(reader.__file__).resolve().parents[1]
        env = {**os.environ, "PYTHONPATH": str(scripts), "PYTHONDONTWRITEBYTECODE": "1"}
        for command in ([sys.executable, str(Path(reader.__file__))],
                        [sys.executable, "-m", "bench.read_replay_fields"]):
            for fault in ("missing", "identity", "suite-shape", "corrupt", "stimulus", "contract"):
                with self.subTest(command=command[1:], fault=fault), tempfile.TemporaryDirectory() as temporary:
                    _image, registration = self.camera_pose(self.radar_frame())
                    run = self.make_run(Path(temporary), registration, declared=True)
                    manifest_path = run / "camera/capture_manifest.json"
                    if fault == "missing":
                        manifest_path.unlink()
                    elif fault in {"identity", "suite-shape"}:
                        manifest = json.loads(manifest_path.read_text())
                        if fault == "identity":
                            manifest["capture_id"] = "invalid"
                        else:
                            manifest["identity"]["suite"] = []
                        manifest_path.write_text(json.dumps(manifest))
                    elif fault in {"stimulus", "contract"}:
                        path = run / ("replay_stimulus.ndjson" if fault == "stimulus" else "replay_display_contract.json")
                        path.write_bytes(path.read_bytes() + b" ")
                    else:
                        path = run / "camera/frame_timing.ndjson"
                        path.write_bytes(path.read_bytes().replace(b"1180000000", b"1180000001"))
                    binary = run / "bin"
                    binary.mkdir()
                    ffmpeg = binary / "ffmpeg"
                    ffmpeg.write_text("#!/bin/sh\nexit 93\n")
                    ffmpeg.chmod(0o755)
                    command_env = {**env, "PATH": str(binary) + os.pathsep + env.get("PATH", "")}
                    output = run / "reading.json"
                    result = subprocess.run([*command, str(run), "--output", str(output)],
                                            cwd=temporary, env=command_env, capture_output=True, text=True)
                    self.assertEqual(2, result.returncode, result.stderr)
                    report = json.loads(output.read_text())
                    self.assertEqual("INCONCLUSIVE", report["result"])
                    self.assertRegex(report["reason"], "(camera|replay).*(artifact|identity|suite)")
                    self.assertNotIn("Traceback", result.stderr)

    def test_comparison_reads_registered_translation_and_scale(self):
        image = self.radar_frame()
        for scale, dx, dy in ((1., 0, 0), (1., 0, 60), (1., -80, -60), (.8, 80, 80)):
            with self.subTest(scale=scale, dx=dx, dy=dy):
                moved, registration = self.camera_pose(image, scale, dx, dy)
                report = self.compare_frame(moved, registration)
                self.assertEqual("PASS", report["result"], report)
                self.assertEqual(1, report["secondaryCardTextDirection"]["matched"])

    def test_registered_comparison_still_detects_wrong_visible_band(self):
        image = self.radar_frame()
        image[335:375, 335:390] = 0
        image[405:445, 335:390] = (240, 240, 240)  # X instead of the requested K.
        moved, registration = self.camera_pose(image, dy=60)
        report = self.compare_frame(moved, registration)
        self.assertEqual("FAIL", report["result"])
        self.assertIn({"sequence": 2, "videoSeconds": 1.18, "field": "band",
                       "expected": "k", "observed": "x", "category": "mismatched"},
                      report["exceptions"])

    def test_comparison_requires_usable_registration_transform(self):
        image = self.radar_frame()
        for crop in (None, [0, 0, 0, .5], [0, 0, 2, .5], [0, 0, float("nan"), .5]):
            with self.subTest(crop=crop), self.assertRaisesRegex(ValueError, "transform|crop"):
                self.compare_frame(image, {"result": "PASS", "transform": {
                    "kind": "dynamic_similarity", "crop_fractions": crop}})

    def test_photo_subtype_is_read_from_pixels_without_expected_input(self):
        image = np.zeros((720, 1280, 3), dtype=np.uint8)
        for index, character in enumerate("MRCT"):
            x = 460 + 19 * index
            image[385:409, x:x + 16][CARD_TEMPLATES[character]] = (200, 40, 170)
        image[393:401, 424:444] = (240, 240, 240)  # Card's SIDE arrow.

        self.assertEqual((True, "MRCT", "SIDE"), read_pixels(image)["cards"][0])

    def test_photo_expectation_uses_subtype_not_frequency(self):
        stimulus = {"expected": {"alerts": [
            {"band": "k", "frequencyMHz": 24125, "photoType": 1,
             "direction": "REAR", "priority": False}]}}
        self.assertEqual([("MRCT", "REAR")], secondary_identities(stimulus))

    def test_numeric_photo_card_cannot_match_subtype(self):
        image = np.zeros((720, 1280, 3), dtype=np.uint8)
        for index, character in enumerate("24.125"):
            x = 488 + 19 * index
            image[385:409, x:x + 16][CARD_TEMPLATES[character]] = (240, 240, 240)
        image[393:401, 424:444] = (240, 240, 240)

        observed = read_pixels(image)["cards"][0]
        self.assertEqual((True, "24.125", "SIDE"), observed)
        self.assertNotEqual(("MRCT", "SIDE"), observed[1:])

    def test_numeric_card_recovers_vertical_camera_shift(self):
        image = np.zeros((720, 1280, 3), dtype=np.uint8)
        for index, character in enumerate("24.150"):
            x = 488 + 19 * index
            image[388:412, x:x + 16][CARD_TEMPLATES[character]] = (240, 240, 240)
        image[393:401, 424:444] = (240, 240, 240)

        self.assertEqual((True, "24.150", "SIDE"), read_pixels(image)["cards"][0])

    def test_post_capture_report_has_distinct_exit_statuses(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "visual_fields_result.json"
            for result, status in (("PASS", 0), ("FAIL", 1), ("INCONCLUSIVE", 2)):
                with (mock.patch.object(sys, "argv", ["reader", "unused", "--output", str(output)]),
                      mock.patch.object(reader.shutil, "which", return_value="ffmpeg"),
                      mock.patch.object(reader, "compare_run", return_value={"result": result})):
                    with self.assertRaises(SystemExit) as exit_result:
                        reader.main()
                self.assertEqual(status, exit_result.exception.code)
                self.assertEqual(result, json.loads(output.read_text())["result"])

    def test_incomplete_raw_capture_cannot_receive_visual_pass(self):
        with tempfile.TemporaryDirectory() as temporary:
            run = Path(temporary)
            (run / "window_result.json").write_text('{"result":"COLLECTION_FAILED"}')
            with self.assertRaisesRegex(ValueError, "not complete and qualified"):
                reader.compare_run(run)


if __name__ == "__main__":
    unittest.main()
