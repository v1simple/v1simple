#!/usr/bin/env python3
"""Focused product-path tests for the staged temporal qualification workflow."""

from __future__ import annotations

import json
import io
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
from argparse import Namespace


SCRIPTS_DIR = Path(__file__).resolve().parent
BENCH_DIR = SCRIPTS_DIR / "bench"
sys.path.insert(0, str(SCRIPTS_DIR))
sys.path.insert(0, str(BENCH_DIR))

import encounter_qualification
import encounter_qualification_workflow as workflow
from encounter_qualification import (
    CLASSIFIER_IMPLEMENTATION_FILES,
    TEMPORAL_SOURCE_HASH_FIELDS,
    TEMPORAL_V2_OBSERVER_RUBRICS,
    temporal_v2_observer_instructions,
)
import test_encounter_qualification as qualification_test_support
from camera_artifacts import build_capture_manifest
from camera_contract import EXPECTED_CAMERA_NAME


class QualificationWorkflowTests(unittest.TestCase):
    def test_manifest_defaults_follow_bench_environment(self):
        with patch.dict(os.environ, {"BENCH_ARTIFACT_ROOT": "/tmp/bench-owned"}, clear=False):
            os.environ.pop("BENCH_ENCOUNTER_QUALIFICATION", None)
            self.assertEqual(
                workflow._default_manifest_path(),
                Path("/tmp/bench-owned/qualification/encounter-reader.json"))
        with patch.dict(os.environ, {
                "BENCH_ARTIFACT_ROOT": "/tmp/ignored",
                "BENCH_ENCOUNTER_QUALIFICATION": "/tmp/exact-reader.json"}, clear=False):
            self.assertEqual(workflow._default_manifest_path(), Path("/tmp/exact-reader.json"))

    @unittest.skipUnless(shutil.which("ffmpeg") and shutil.which("ffprobe"),
                         "ffmpeg and ffprobe are required")
    def test_lossless_observer_media_is_bound_to_retained_video_and_sidecar(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            camera = root / "capture"
            camera.mkdir()
            video = camera / "camera.mkv"
            subprocess.run([
                shutil.which("ffmpeg"), "-nostdin", "-hide_banner", "-loglevel", "error",
                "-f", "lavfi", "-i", "testsrc2=size=1280x720:rate=200", "-frames:v", "12",
                "-an", "-c:v", "ffv1", str(video),
            ], check=True)
            registration = {"result": "PASS", "landmark_bounds": [376, 192, 595, 270]}
            workflow.write_json(camera / "camera_preflight.json", {
                "schema_version": 1, "kind": "bench_camera_preflight", "result": "PASS",
                "registration": registration,
            })
            for name in ("session.png", "bright.png", "dim.png"):
                (camera / name).write_bytes(b"retained still " + name.encode("ascii"))
            rows = []
            for index in range(12):
                rows.append({
                    "schema_version": 1, "phase": "recording", "frame_seq": index + 1,
                    "source_clock": "avcapture_session_synchronization_clock",
                    "callback_clock": "host_monotonic", "source_pts_value": index,
                    "source_pts_timescale": 200, "source_duration_value": 1,
                    "source_duration_timescale": 200,
                    "callback_host_ns": 1_000_000_000 + index * 5_000_000,
                    "host_capture_ns": 1_000_000_000 + index * 5_000_000,
                    "video_pts_value": index, "video_pts_timescale": 200,
                    "video_duration_value": 1, "video_duration_timescale": 200,
                    "duration_ns": 5_000_000, "status": "written", "drop_reason": None,
                    "timestamp_error": None, "timestamp_errors": [],
                })
            sidecar = camera / "frame_timing.ndjson"
            sidecar.write_text("".join(json.dumps(row) + "\n" for row in rows),
                               encoding="utf-8")
            timing = {
                "schema_version": 1, "kind": "camera_video_timing_verification",
                "status": "verified", "source_frame_count": 12,
                "written_frame_count": 12, "encoded_frame_count": 12,
                "timestamp_error_count": 0, "missing_encoded_frame_count": 0,
                "extra_encoded_frame_count": 0, "duration_mismatch_count": 0,
                "maximum_source_interval_ns": 5_000_000,
            }
            workflow.write_json(camera / "video_timing_verification.json", timing)
            camera_result = {
                "result": "CAPTURED", "camera_name": EXPECTED_CAMERA_NAME,
                "camera_device_index": 0,
                "profile": {"framerate": 200, "video_size": "1280x720"},
                "expected_duration_seconds": 0.06, "video_duration_seconds": 0.06,
                "timestamp_utc": "2026-09-04T00:00:00Z", "video": video.name,
                "video_probe": {"width": 1280, "height": 720},
                "frame_timing": sidecar.name,
                "video_timing_verification": "video_timing_verification.json",
                "video_timing_verification_result": timing,
                "session_start_still": "session.png", "bright_still": "bright.png",
                "dim_still": "dim.png",
            }
            capture_manifest = build_capture_manifest(
                camera_dir=camera, camera_result=camera_result, suite="replay")
            workflow.write_json(camera / "capture_manifest.json", capture_manifest)

            observer = root / "observer_packet"
            clip = observer / "clips/OPAQUE123456.mov"
            source_indices, size = workflow._build_clip(
                video, clip, "v1-main-bar-adjacent-redraw-v1", [4, 5, 6], 12,
                registration, 1280, 720)
            item = {
                "opaque_id": "OPAQUE123456", "clip": "clips/OPAQUE123456.mov",
                "sha256": workflow.sha256(clip), "size_bytes": size,
                "target_run_video_indices": [4, 5, 6],
                "clip_source_video_indices": source_indices,
                "target_run_clip_frame_indices": [source_indices.index(value)
                                                   for value in (4, 5, 6)],
                "full_run_video_indices": [4, 5, 6],
                "full_run_clip_frame_indices": [source_indices.index(value)
                                                 for value in (4, 5, 6)],
                "inset_source_box": list(workflow._raw_box(
                    workflow._logical_inset("v1-main-bar-adjacent-redraw-v1"),
                    registration, 1280, 720)),
            }
            manifest_path = observer / "manifest.json"
            workflow.write_json(manifest_path, {"items": [item]})
            selection = {"samples": [{
                "video_frame_index": index, "source_frame_seq": index + 1,
                "capture_ns": 1_000_000_000 + index * 5_000_000,
            } for index in (4, 5, 6)]}
            paths = {
                "capture_manifest": camera / "capture_manifest.json",
                "qualification_video": video,
                "frame_timing": sidecar,
                "video_timing_verification": camera / "video_timing_verification.json",
                "observer_manifest": manifest_path,
            }
            source_rows = encounter_qualification._validate_temporal_v2_media(
                "v1-main-bar-adjacent-redraw-v1", paths,
                {"video_timing_verification": timing},
                {"camera": {"capture_id": capture_manifest["capture_id"],
                            "video_timing_verification_result": timing}},
                selection, [item])
            self.assertEqual(source_rows[5], {
                "source_frame_seq": 6, "capture_ns": 1_025_000_000})

            # Re-sealing arbitrary bytes cannot turn them into source-bound pixels.
            clip.write_bytes(b"resealed garbage clip")
            item["sha256"] = hashlib.sha256(clip.read_bytes()).hexdigest()
            item["size_bytes"] = clip.stat().st_size
            with self.assertRaisesRegex(
                    encounter_qualification.QualificationError, "clip probe failed"):
                encounter_qualification._validate_temporal_v2_media(
                    "v1-main-bar-adjacent-redraw-v1", paths,
                    {"video_timing_verification": timing},
                    {"camera": {"capture_id": capture_manifest["capture_id"],
                                "video_timing_verification_result": timing}},
                    selection, [item])

    def test_build_clip_retains_twenty_context_frames_and_slows_for_review(self):
        with tempfile.TemporaryDirectory() as temporary:
            destination = Path(temporary) / "clip.mov"
            commands = []

            def run(command, **_kwargs):
                commands.append(command)
                Path(command[-1]).write_bytes(b"observer clip")

            probe = {"streams": [{
                "codec_name": "png", "pix_fmt": "rgb24", "width": 1720, "height": 720,
                "nb_frames": "43", "r_frame_rate": "25/1",
            }]}
            with (patch.object(workflow.shutil, "which", side_effect=lambda name: f"/{name}"),
                  patch.object(workflow.subprocess, "run", side_effect=run),
                  patch.object(workflow.subprocess, "check_output",
                               return_value=json.dumps(probe))):
                source, size = workflow._build_clip(
                    Path("reserved.mp4"), destination,
                    "v1-main-bar-adjacent-redraw-v1", [50, 51, 52], 100,
                    {"landmark_bounds": [376, 192, 595, 270]}, 1280, 720)

            self.assertEqual(source, list(range(30, 73)))
            self.assertEqual(size, len(b"observer clip"))
            graph = commands[0][commands[0].index("-filter_complex") + 1]
            self.assertIn("trim=start_frame=30:end_frame=73", graph)
            self.assertIn("setpts=N/(25*TB)", graph)

    def test_prepare_classifier_output_passes_the_real_v2_verifier(self):
        helper = qualification_test_support.QualificationTests(
            "test_complete_exact_bundle_qualifies")
        helper.setUp()
        self.addCleanup(helper.tearDown)
        classifier = "v1-main-bar-adjacent-redraw-v1"
        seeded_temporal, seeded = helper.generic_temporal_validation(classifier)
        seeded_entry = seeded_temporal[classifier]
        spec_path = helper.root / seeded_entry["spec"]["path"]
        sources = seeded["source_paths"]
        hidden = json.loads(sources["restricted_hidden_key"].read_text(encoding="utf-8"))
        candidates = [{
            "decision": item["frozen_classifier_decision"],
            "record": item["frozen_classifier_record"],
            "indices": item["target_run_video_indices"],
            "full_indices": item["full_run_video_indices"],
        } for item in hidden["items"]]
        implementation = {
            name: helper.method[name] for name in CLASSIFIER_IMPLEMENTATION_FILES[classifier]
        }
        reader_binding = {
            "method_version": qualification_test_support.READER["method_version"],
            "source_sha256": helper.method["encounter_reader.py"],
            "runtime_sha256": qualification_test_support.canonical_digest(
                qualification_test_support.READER),
            "bench_source_sha256": "c" * 64,
        }
        campaign = {
            "reader_runtime": qualification_test_support.READER,
            "reader_binding": reader_binding,
            "implementation_sha256": helper.method,
            "classifiers": {
                classifier: {
                    "spec": helper.reference(spec_path, helper.root),
                    "pre_pixel_freeze": helper.reference(
                        sources["pre_pixel_freeze"], helper.root),
                    "observer_readme": helper.reference(sources["observer_readme"], helper.root),
                    "implementation_sha256": implementation,
                    "observer_rubric_sha256": json.loads(
                        sources["observer_manifest"].read_text(encoding="utf-8")
                    )["observer_rubric_sha256"],
                },
            },
        }
        window = json.loads(sources["window_result"].read_text(encoding="utf-8"))
        stage = helper.root / "workflow-prepared"
        stage.mkdir()
        opaque_ids = [item["opaque_id"] for item in hidden["items"]]

        def build_clip(_video, destination, _classifier, target, frame_count,
                       _registration, _width, _height):
            first, last = max(0, target[0] - 20), min(frame_count - 1, target[-1] + 20)
            source_indices = list(range(first, last + 1))
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(f"workflow clip {destination.stem}".encode("ascii"))
            return source_indices, destination.stat().st_size

        fake_data = {
            "video": Path("reserved.mp4"),
            "retained_capture": helper.root / "retained-capture",
            "timing": {"encoded_frame_count": 100},
            "registration": {"landmark_bounds": [376, 192, 595, 270]},
            "width": 1280,
            "height": 720,
            "analysis_result_path": sources["analysis_result"],
        }

        def link_capture(_retained, destination):
            destination.mkdir(parents=True)
            artifacts = {
                "video": {"path": "camera.mov"},
                "frame_timing": {"path": "frame_timing.ndjson"},
                "video_timing_verification": {"path": "video_timing_verification.json"},
            }
            (destination / "camera.mov").write_bytes(b"retained video")
            (destination / "frame_timing.ndjson").write_text("retained sidecar\n", encoding="utf-8")
            workflow.write_json(
                destination / "video_timing_verification.json", {"status": "verified"})
            manifest = {"identity": {"artifacts": artifacts}}
            workflow.write_json(destination / "capture_manifest.json", manifest)
            return manifest

        with (patch.dict(workflow.SPEC_BY_CLASSIFIER, {classifier: spec_path}),
              patch.object(workflow, "_opaque_ids", return_value=opaque_ids),
              patch.object(workflow.random.SystemRandom, "shuffle", lambda _self, _items: None),
              patch.object(workflow, "_link_capture_view", side_effect=link_capture),
              patch.object(workflow, "_build_clip", side_effect=build_clip)):
            workflow._prepare_classifier(
                stage, helper.root, campaign, classifier, candidates,
                sources["analysis_selection"], sources["qualification_capture"],
                sources["window_result"], window, fake_data)

        classifier_root = stage / "classifiers" / classifier
        workflow_sources = workflow._source_paths(classifier_root)
        observation_template = workflow.read_json(
            workflow_sources["completed_observations"])
        self.assertEqual(
            observation_template["blind_protocol"], workflow.BLIND_PROTOCOL_TEMPLATE)
        shutil.copyfile(sources["completed_observations"],
                        workflow_sources["completed_observations"])
        manifest = workflow.read_json(workflow_sources["observer_manifest"])
        prepared_hidden = workflow.read_json(workflow_sources["restricted_hidden_key"])
        completed = workflow.read_json(workflow_sources["completed_observations"])
        comparison = workflow._matrix_document(
            classifier, seeded_entry["classifier_spec_sha256"], workflow_sources,
            manifest, completed, prepared_hidden)
        comparison_path = classifier_root / "comparison.json"
        workflow.write_json(comparison_path, comparison)
        entry = {
            "classifier_spec_sha256": seeded_entry["classifier_spec_sha256"],
            "spec": helper.reference(classifier_root / "spec.json", helper.root),
            "validation": helper.reference(comparison_path, helper.root),
            "source_artifacts": {
                name: workflow.reference(path, classifier_root)
                for name, path in workflow_sources.items()
            },
        }
        result = helper.verify(helper.write_bundle(temporal={classifier: entry}))
        self.assertEqual(result["status"], "QUALIFIED", result["errors"])
        self.assertEqual(set(workflow_sources), set(TEMPORAL_SOURCE_HASH_FIELDS))

        packet = classifier_root / "source" / "observer_packet"
        packet_files = {str(path.relative_to(packet)) for path in packet.rglob("*") if path.is_file()}
        self.assertEqual(
            packet_files,
            {"README.txt", "manifest.json", "observations.json",
             *{f"clips/{opaque_id}.mov" for opaque_id in opaque_ids}},
        )
        self.assertEqual(
            (packet / "README.txt").read_text(encoding="utf-8"),
            temporal_v2_observer_instructions(classifier),
        )
        for item in manifest["items"]:
            source_indices = item["clip_source_video_indices"]
            target_indices = item["target_run_video_indices"]
            self.assertEqual(
                item["target_run_clip_frame_indices"],
                [source_indices.index(value) for value in target_indices],
            )

    def test_matrix_marks_indeterminate_rejections_as_abstentions(self):
        helper = qualification_test_support.QualificationTests(
            "test_complete_exact_bundle_qualifies")
        helper.setUp()
        self.addCleanup(helper.tearDown)
        for classifier in TEMPORAL_V2_OBSERVER_RUBRICS:
            with self.subTest(classifier=classifier):
                temporal, context = helper.generic_temporal_validation(
                    classifier, indeterminate_rejects=True)
                paths = context["source_paths"]
                document = workflow._matrix_document(
                    classifier, temporal[classifier]["classifier_spec_sha256"], paths,
                    workflow.read_json(paths["observer_manifest"]),
                    workflow.read_json(paths["completed_observations"]),
                    workflow.read_json(paths["restricted_hidden_key"]),
                )
                self.assertEqual(document["confusion_matrix"]["true_reject"], 0)
                self.assertEqual(document["confusion_matrix"]["abstain"], 5)
                self.assertEqual(document["denominators"]["observer_indeterminate"], 5)
                self.assertFalse(document["integrity_pass"])
                self.assertFalse(document["allowlist_decision"]["allowlist_exact_classifier"])
    def test_atomic_publish_restores_both_files_when_postcheck_fails(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            policy = root / "policy.json"
            manifest = root / "manifest.json"
            policy.write_bytes(b"old policy")
            manifest.write_bytes(b"old manifest")

            def reject_publish():
                raise workflow.WorkflowError("post-publish verification failed")

            with patch.object(workflow, "POLICY_PATH", policy):
                with self.assertRaisesRegex(workflow.WorkflowError, "post-publish"):
                    workflow._atomic_publish(
                        b"new policy", b"new manifest", manifest, reject_publish)
            self.assertEqual(policy.read_bytes(), b"old policy")
            self.assertEqual(manifest.read_bytes(), b"old manifest")

    def test_interrupted_manifest_first_publish_recovers_idempotently(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            policy = root / "policy.json"
            manifest = root / "manifest.json"
            policy.write_bytes(b"old policy")
            manifest.write_bytes(b"old manifest")
            with patch.object(workflow, "POLICY_PATH", policy):
                marker, _transaction = workflow._publish_transaction(
                    b"new policy", b"new manifest", manifest)
                # Exact recoverable crash state: manifest was installed first,
                # policy still keeps the new qualification fail closed.
                workflow._durable_replace(manifest, b"new manifest")
                checked = workflow._recover_publish(
                    manifest,
                    lambda: {"status": "QUALIFIED"}
                    if (manifest.read_bytes(), policy.read_bytes()) ==
                       (b"new manifest", b"new policy")
                    else (_ for _ in ()).throw(AssertionError("partial publish")))
            self.assertEqual(checked, {"status": "QUALIFIED"})
            self.assertEqual(policy.read_bytes(), b"new policy")
            self.assertEqual(manifest.read_bytes(), b"new manifest")
            self.assertFalse(marker.exists())

    def test_consumed_marker_cannot_be_reused(self):
        with tempfile.TemporaryDirectory() as temporary:
            marker = Path(temporary) / workflow.CONSUMED_NAME
            workflow._write_exclusive_json(marker, {"state": "consumed"})
            with self.assertRaisesRegex(workflow.WorkflowError, "already consumed"):
                workflow._write_exclusive_json(marker, {"state": "replacement"})
            self.assertEqual(workflow.read_json(marker), {"state": "consumed"})

    def test_cli_reports_qualification_rejection_without_traceback(self):
        class Parser:
            @staticmethod
            def parse_args():
                return Namespace(command="prepare", campaign=Path("campaign"),
                                 run_dir=Path("run"))

        error = encounter_qualification.QualificationError("capture boundary rejected")
        stderr = io.StringIO()
        with (patch.object(workflow, "build_parser", return_value=Parser()),
              patch.object(workflow, "prepare", side_effect=error),
              patch.object(sys, "stderr", stderr)):
            self.assertEqual(workflow.main(), 2)
        self.assertEqual(
            stderr.getvalue(),
            "qualification workflow failed: capture boundary rejected\n",
        )


if __name__ == "__main__":
    unittest.main()
