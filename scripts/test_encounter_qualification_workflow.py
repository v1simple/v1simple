#!/usr/bin/env python3
"""Focused product-path tests for the staged temporal qualification workflow."""

from __future__ import annotations

import json
import io
import hashlib
import os
from contextlib import ExitStack, contextmanager
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
    COMMON_TEMPORAL_IMPLEMENTATION_FILES,
    STATIC_READER_IMPLEMENTATION_FILES,
    TEMPORAL_SOURCE_HASH_FIELDS,
    TEMPORAL_V2_OBSERVER_RUBRICS,
    temporal_v2_observer_instructions,
)
import test_encounter_qualification as qualification_test_support
from camera_artifacts import build_capture_manifest
from camera_contract import EXPECTED_CAMERA_NAME


class QualificationWorkflowTests(unittest.TestCase):
    def test_reanalysis_runtime_mismatch_stops_before_pixel_reading(self):
        with tempfile.TemporaryDirectory() as directory:
            prepared = Path(directory)
            inventory = prepared / "replay-input-manifest.json"
            inventory.write_text("{}\n")
            document = {"replay_input_manifest_sha256": workflow.sha256(inventory)}
            campaign = {"reader_runtime": {"method_version": 5, "ocr_available": True}}
            with (patch.object(workflow, "_verify_replay_inputs"),
                  patch.object(workflow, "reader_runtime", return_value={
                      "method_version": 5, "ocr_available": False}),
                  patch("encounter_check.analyze") as pixels):
                with self.assertRaisesRegex(workflow.WorkflowError,
                                            "runtime differs before pixel reading"):
                    workflow._rederive_analysis(prepared, document, campaign)
            pixels.assert_not_called()
            self.assertEqual(list(prepared.glob(".qualification-reanalysis-*")), [])

    def test_reanalysis_explicitly_requests_only_frozen_candidate_classifiers(self):
        with tempfile.TemporaryDirectory() as directory:
            prepared = Path(directory)
            inventory = prepared / "replay-input-manifest.json"
            inventory.write_text("{}\n")
            selection_bytes = b"{}\n"
            document = {
                "replay_input_manifest_sha256": workflow.sha256(inventory),
                "analysis_selection_sha256": hashlib.sha256(selection_bytes).hexdigest(),
            }
            campaign = {"reader_runtime": {"method_version": 5},
                        "implementation_sha256": {},
                        "classifiers": {name: {} for name in workflow.TARGET_CLASSIFIERS}}
            calls = []

            def analyze(_run, output, _ranges, _cadence, **options):
                calls.append(options)
                (output / "selection.json").write_bytes(selection_bytes)
                return {"errors": [], "temporal_classification": {"errors": []},
                        "evidence": {"reader": campaign["reader_runtime"]},
                        "implementation_sha256": {}}

            with (patch.object(workflow, "_verify_replay_inputs"),
                  patch.object(workflow, "reader_runtime",
                               return_value=campaign["reader_runtime"]),
                  patch("encounter_check.analyze", side_effect=analyze)):
                workflow._rederive_analysis(prepared, document, campaign)
            self.assertEqual(calls, [{
                "inspect_transitions": True,
                "reader_qualification": None,
                "temporal_classifier_ids": tuple(sorted(workflow.TARGET_CLASSIFIERS)),
            }])

    def test_capture_context_is_checked_before_retention_or_pixel_analysis(self):
        import encounter_reader
        from encounter_check import analyze

        campaign = {
            "reader_runtime": {"method_version": encounter_reader.METHOD_VERSION},
            "implementation_sha256": workflow.method_hashes(),
            "classifiers": {name: {} for name in workflow.TARGET_CLASSIFIERS},
        }
        window = {"camera": {
            "capture_id": "a" * 64,
            "video_timing_verification_result": {"maximum_source_interval_ns": 15_000_000},
        }}
        workflow._validate_capture_classifier_contexts(campaign, window)
        campaign["reader_runtime"]["method_version"] = -1
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with (patch.object(workflow, "_campaign", return_value=(root, campaign)),
                  patch.object(workflow, "_verify_frozen_source"),
                  patch.object(workflow, "_validate_capture", return_value=({}, window)),
                  patch.object(workflow, "_retain_replay_run") as retain,
                  patch("encounter_check.analyze", wraps=analyze) as pixels):
                with self.assertRaisesRegex(workflow.WorkflowError, "capture classifier context rejected"):
                    workflow.prepare(root, root / "run")
            retain.assert_not_called()
            pixels.assert_not_called()
            self.assertFalse((root / "prepared").exists())

    def test_base_manifest_is_pinned_and_drift_stops_before_observation_access(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            destination = root / "campaign"
            base = root / "base.json"
            base.write_text('{"qualification_id":"original"}')
            expected = workflow.sha256(base)
            with (patch.object(workflow, "git_identity", return_value=("a" * 40, True)),
                  patch.object(workflow, "reader_runtime", return_value={"method_version": 5}),
                  patch.object(workflow, "validate_base_evidence")):
                workflow.freeze(destination, base)
            campaign = workflow.read_json(destination / "campaign.json")
            self.assertEqual(campaign["base_manifest_sha256"], expected)
            base.write_text('{"qualification_id":"replacement"}')
            with (patch.object(workflow, "_verify_frozen_source"),
                  patch.object(workflow, "_completed_observations") as observations):
                with self.assertRaisesRegex(workflow.WorkflowError, "base qualification changed"):
                    workflow.finalize(destination, base, root / "published.json")
            observations.assert_not_called()
            self.assertFalse((destination / "prepared" / workflow.CONSUMED_NAME).exists())

    def test_failed_base_preflight_does_not_reserve_a_campaign(self):
        with tempfile.TemporaryDirectory() as directory:
            destination = Path(directory) / "campaign"
            base = Path(directory) / "base.json"
            with (patch.object(workflow, "git_identity", return_value=("a" * 40, True)),
                  patch.object(workflow, "reader_runtime", return_value={"method_version": 5}),
                  patch.object(workflow, "validate_base_evidence",
                               side_effect=workflow.WorkflowError("retained base rejected")) as verify):
                with self.assertRaisesRegex(workflow.WorkflowError, "retained base rejected"):
                    workflow.freeze(destination, base)
            self.assertFalse(destination.exists())
            verify.assert_called_once()
            self.assertEqual(verify.call_args.args[0], base.resolve())

    def test_workflow_and_analysis_bind_every_required_runtime_source(self):
        from encounter_check import analyze
        frozen = workflow.method_hashes()
        static = workflow.static_method_hashes(frozen)
        self.assertEqual(set(static), set(STATIC_READER_IMPLEMENTATION_FILES))
        self.assertIn("encounter_qualification.py", static)
        self.assertLessEqual(
            {"encounter_assessment.py", "visual_compare.py", "artifact_privacy.py"},
            set(COMMON_TEMPORAL_IMPLEMENTATION_FILES))
        self.assertNotIn("encounter_bar_transition.py", static)
        self.assertNotIn(workflow.POLICY_PATH.name, static)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            # A missing run stops before pixels, while retaining the real method inventory.
            result = analyze(root / "missing-run", root, None, 2)
            self.assertTrue(result["errors"])
            for name in STATIC_READER_IMPLEMENTATION_FILES:
                expected = hashlib.sha256((BENCH_DIR / name).read_bytes()).hexdigest()
                self.assertEqual(frozen[name], expected)
                self.assertEqual(static[name], expected)
                self.assertEqual(result["implementation_sha256"][name], expected)
                self.assertEqual((root / "method" / name).read_bytes(), (BENCH_DIR / name).read_bytes())

    def test_workflow_targets_only_physically_supported_temporal_candidates(self):
        self.assertEqual(
            workflow.TARGET_CLASSIFIERS,
            (
                "v1-arrow-phase-edge-v3",
                "v1-arrow-target-acquisition-v1",
                "v1-stable-frequency-closed-context-v3",
                "v1-secondary-closed-context-v2",
                "v1-secondary-text-optical-bridge-v1",
            ),
        )
        self.assertNotIn(
            "v1-unmute-stable-frequency-sweep-v2",
            workflow.classifier_identity(),
        )
        self.assertIn(workflow.ARROW_CLASSIFIER_ID, workflow.classifier_identity())
        self.assertIn(
            workflow.ARROW_ACQUISITION_CLASSIFIER_ID, workflow.classifier_identity())
        self.assertIn(
            workflow.SECONDARY_CONTEXT_CLASSIFIER_ID, workflow.classifier_identity())
        self.assertIn(
            workflow.SECONDARY_OPTICAL_CLASSIFIER_ID, workflow.classifier_identity())
        self.assertNotIn("v1-main-bar-adjacent-redraw-v2", workflow.TARGET_CLASSIFIERS)
        self.assertNotIn("v1-muted-badge-rising-fill-v2", workflow.TARGET_CLASSIFIERS)
        self.assertNotIn("v1-unmute-stable-frequency-sweep-v2", workflow.TARGET_CLASSIFIERS)

    def test_arrow_acquisition_uses_its_exact_spec_and_implementation(self):
        classifier = workflow.ARROW_ACQUISITION_CLASSIFIER_ID
        identity = workflow.classifier_identity()[classifier]
        self.assertEqual(identity, (
            classifier, workflow.sha256(workflow.SPEC_BY_CLASSIFIER[classifier])))
        self.assertEqual(
            CLASSIFIER_IMPLEMENTATION_FILES[classifier],
            (*COMMON_TEMPORAL_IMPLEMENTATION_FILES,
             "encounter_arrow_acquisition.py"))

    def test_arrow_acquisition_capture_context_dispatches_to_its_classifier(self):
        import encounter_arrow_acquisition
        import encounter_reader

        classifier = workflow.ARROW_ACQUISITION_CLASSIFIER_ID
        campaign = {
            "reader_runtime": {"method_version": encounter_reader.METHOD_VERSION},
            "implementation_sha256": workflow.method_hashes(),
            "classifiers": {classifier: {}},
        }
        window = {"camera": {
            "capture_id": "a" * 64,
            "video_timing_verification_result": {
                "status": "verified", "maximum_source_interval_ns": 5_000_000},
        }}
        with patch(
                "encounter_arrow_acquisition.classify_arrow_acquisition_runs",
                wraps=encounter_arrow_acquisition.classify_arrow_acquisition_runs) as classify:
            workflow._validate_capture_classifier_contexts(campaign, window)
        classify.assert_called_once()

    def test_arrow_acquisition_uses_complete_transition_for_observer_context(self):
        classifier = workflow.ARROW_ACQUISITION_CLASSIFIER_ID
        record = {
            "classifier_id": classifier,
            "video_frame_indices": [11, 12],
            "full_transition_indices": [10, 11, 12, 13],
        }
        candidates = workflow._candidate_records(
            {"classifications": [record], "rejected_runs": []}, classifier)
        self.assertEqual(candidates[0]["indices"], [11, 12])
        self.assertEqual(candidates[0]["full_indices"], [10, 11, 12, 13])
        self.assertEqual(
            workflow._logical_inset(classifier),
            workflow._logical_inset(workflow.ARROW_CLASSIFIER_ID))

    def test_frequency_context_uses_exact_dependencies_context_and_inset(self):
        classifier = workflow.FREQUENCY_CONTEXT_CLASSIFIER_ID
        identity = workflow.classifier_identity()[classifier]
        self.assertEqual(identity, (
            classifier, workflow.sha256(workflow.SPEC_BY_CLASSIFIER[classifier])))
        self.assertEqual(CLASSIFIER_IMPLEMENTATION_FILES[classifier], (
            *COMMON_TEMPORAL_IMPLEMENTATION_FILES,
            "encounter_frequency_context.py", "encounter_redraw_probe.py"))
        record = {
            "classifier_id": classifier,
            "video_frame_indices": [11, 12],
            "context_frame_indices": [10, 11, 12, 13],
        }
        candidates = workflow._candidate_records(
            {"classifications": [record], "rejected_runs": []}, classifier)
        self.assertEqual(candidates[0]["indices"], [11, 12])
        self.assertEqual(candidates[0]["full_indices"], [10, 11, 12, 13])
        self.assertEqual(
            workflow._logical_inset(classifier),
            workflow._logical_inset("v1-unmute-stable-frequency-sweep-v2"))

    def test_frequency_context_capture_context_dispatches_to_its_classifier(self):
        import encounter_frequency_context
        import encounter_reader

        classifier = workflow.FREQUENCY_CONTEXT_CLASSIFIER_ID
        campaign = {
            "reader_runtime": {"method_version": encounter_reader.METHOD_VERSION},
            "implementation_sha256": workflow.method_hashes(),
            "classifiers": {classifier: {}},
        }
        window = {"camera": {
            "capture_id": "a" * 64,
            "video_timing_verification_result": {
                "status": "verified", "maximum_source_interval_ns": 5_000_000},
        }}
        with patch(
                "encounter_frequency_context.classify_frequency_context_runs",
                wraps=encounter_frequency_context.classify_frequency_context_runs) as classify:
            workflow._validate_capture_classifier_contexts(campaign, window)
        classify.assert_called_once()

    def test_secondary_context_uses_exact_dependencies_context_and_inset(self):
        classifier = workflow.SECONDARY_CONTEXT_CLASSIFIER_ID
        identity = workflow.classifier_identity()[classifier]
        self.assertEqual(identity, (
            classifier, workflow.sha256(workflow.SPEC_BY_CLASSIFIER[classifier])))
        self.assertEqual(CLASSIFIER_IMPLEMENTATION_FILES[classifier], (
            *COMMON_TEMPORAL_IMPLEMENTATION_FILES, "encounter_secondary_context.py"))
        record = {
            "classifier_id": classifier,
            "video_frame_indices": [11, 12],
            "full_context_indices": [9, 10, 11, 12, 13, 14],
        }
        candidates = workflow._candidate_records(
            {"classifications": [record], "rejected_runs": []}, classifier)
        self.assertEqual(candidates[0]["indices"], [11, 12])
        self.assertEqual(candidates[0]["full_indices"], [9, 10, 11, 12, 13, 14])
        self.assertEqual(workflow._logical_inset(classifier), (385, 360, 880, 460))

    def test_secondary_context_capture_context_dispatches_to_its_classifier(self):
        import encounter_reader
        import encounter_secondary_context

        classifier = workflow.SECONDARY_CONTEXT_CLASSIFIER_ID
        campaign = {
            "reader_runtime": {"method_version": encounter_reader.METHOD_VERSION},
            "implementation_sha256": workflow.method_hashes(),
            "classifiers": {classifier: {}},
        }
        window = {"camera": {
            "capture_id": "a" * 64,
            "video_timing_verification_result": {
                "status": "verified", "maximum_source_interval_ns": 5_000_000},
        }}
        with patch(
                "encounter_secondary_context.classify_secondary_context_runs",
                wraps=encounter_secondary_context.classify_secondary_context_runs) as classify:
            workflow._validate_capture_classifier_contexts(campaign, window)
        classify.assert_called_once()

    def test_secondary_optical_uses_exact_dependencies_context_and_inset(self):
        classifier = workflow.SECONDARY_OPTICAL_CLASSIFIER_ID
        identity = workflow.classifier_identity()[classifier]
        self.assertEqual(identity, (
            classifier, workflow.sha256(workflow.SPEC_BY_CLASSIFIER[classifier])))
        self.assertEqual(CLASSIFIER_IMPLEMENTATION_FILES[classifier], (
            *COMMON_TEMPORAL_IMPLEMENTATION_FILES,
            "encounter_secondary_optical_bridge.py", "encounter_secondary_probe.py"))
        record = {
            "classifier_id": classifier,
            "video_frame_indices": [11],
            "left_support": [
                {"video_frame_index": 9}, {"video_frame_index": 10}],
            "right_support": [
                {"video_frame_index": 12}, {"video_frame_index": 13}],
        }
        candidates = workflow._candidate_records(
            {"classifications": [record], "rejected_runs": []}, classifier)
        self.assertEqual(candidates[0]["indices"], [11])
        self.assertEqual(candidates[0]["full_indices"], [9, 10, 11, 12, 13])
        self.assertEqual(workflow._logical_inset(classifier), (385, 360, 880, 460))

    def test_secondary_optical_capture_context_dispatches_to_its_classifier(self):
        import encounter_reader
        import encounter_secondary_optical_bridge

        classifier = workflow.SECONDARY_OPTICAL_CLASSIFIER_ID
        campaign = {
            "reader_runtime": {"method_version": encounter_reader.METHOD_VERSION},
            "implementation_sha256": workflow.method_hashes(),
            "classifiers": {classifier: {}},
        }
        window = {"camera": {
            "capture_id": "a" * 64,
            "video_timing_verification_result": {
                "status": "verified", "maximum_source_interval_ns": 5_000_000},
        }}
        with patch(
                "encounter_secondary_optical_bridge.classify_secondary_optical_bridge",
                wraps=encounter_secondary_optical_bridge.classify_secondary_optical_bridge
                ) as classify:
            workflow._validate_capture_classifier_contexts(campaign, window)
        classify.assert_called_once()

    def test_same_field_rejections_require_and_use_classifier_provenance(self):
        target = workflow.ARROW_ACQUISITION_CLASSIFIER_ID
        point = lambda index: {"video_frame_index": index}
        records = [{
            "event_id": "one", "classifier_id": workflow.ARROW_CLASSIFIER_ID,
            "field": "main_arrows", "code": "OTHER", "first": point(1),
            "last": point(2), "reason": "other classifier",
        }, {
            "event_id": "two", "classifier_id": target,
            "field": "main_arrows", "code": "OWN", "first": point(4),
            "last": point(5), "reason": "target classifier",
        }]
        candidates = workflow._candidate_records(
            {"classifications": [], "rejected_runs": records}, target)
        self.assertEqual(len(candidates), 1)
        self.assertEqual(candidates[0]["record"]["classifier_id"], target)
        del records[0]["classifier_id"]
        with self.assertRaisesRegex(workflow.WorkflowError, "classifier provenance"):
            workflow._candidate_records(
                {"classifications": [], "rejected_runs": records}, target)

        secondary_target = workflow.SECONDARY_OPTICAL_CLASSIFIER_ID
        secondary_records = [{
            "event_id": "context", "classifier_id": workflow.SECONDARY_CONTEXT_CLASSIFIER_ID,
            "field": "secondary", "code": "OTHER", "first": point(7),
            "last": point(7), "reason": "other secondary classifier",
        }, {
            "event_id": "optical", "classifier_id": secondary_target,
            "field": "secondary", "code": "OWN", "first": point(9),
            "last": point(9), "reason": "target secondary classifier",
        }]
        candidates = workflow._candidate_records(
            {"classifications": [], "rejected_runs": secondary_records},
            secondary_target)
        self.assertEqual(len(candidates), 1)
        self.assertEqual(candidates[0]["record"]["event_id"], "optical")

    def test_arrow_selection_is_explicit_and_frozen_in_the_campaign(self):
        import encounter_reader

        arrow = workflow.ARROW_CLASSIFIER_ID
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            destination = root / "campaign"
            base = root / "static-only.json"
            workflow.write_json(base, {"kind": "encounter_reader_qualification",
                                       "temporal_classifiers": {}})
            with (patch.object(workflow, "git_identity", return_value=("a" * 40, True)),
                  patch.object(workflow, "reader_runtime", return_value={
                      "method_version": encounter_reader.METHOD_VERSION}),
                  patch.object(workflow, "validate_base_evidence") as verify):
                workflow.freeze(destination, base, classifier_ids=[arrow])
            campaign = workflow.read_json(destination / "campaign.json")
            self.assertEqual(workflow._campaign_classifiers(campaign), (arrow,))
            self.assertEqual(campaign["base_manifest_sha256"], workflow.sha256(base))
            self.assertEqual(campaign["implementation_sha256"], workflow.method_hashes())
            self.assertGreater(
                set(campaign["implementation_sha256"]),
                set(STATIC_READER_IMPLEMENTATION_FILES))
            self.assertEqual(set(campaign["classifiers"]), {arrow})
            self.assertEqual(verify.call_args.args[0], base.resolve())
            frozen = campaign["classifiers"][arrow]
            self.assertEqual(frozen["spec"]["sha256"],
                             workflow.sha256(workflow.SPEC_BY_CLASSIFIER[arrow]))
            self.assertEqual(frozen["implementation_sha256"], {
                name: workflow.method_hashes()[name]
                for name in CLASSIFIER_IMPLEMENTATION_FILES[arrow]})
        for values in ([], [arrow, arrow], ["invented-classifier"]):
            with self.subTest(values=values), self.assertRaises(workflow.WorkflowError):
                workflow._selected_classifiers(values)

    def test_static_only_base_still_requires_full_qualification_verification(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            camera = {"name": "test camera", "profile": {}}
            base = {"kind": "encounter_reader_qualification", "temporal_classifiers": {},
                    "reader": {"runtime": {}}, "camera": camera}
            for name in ("field_validation", "visible_secondary_validation", "fault_controls"):
                source = root / f"{name}.json"
                source.write_text("{}\n")
                base[name] = workflow.reference(source, root)
            manifest = root / "manifest.json"
            workflow.write_json(manifest, base)
            static, temporal = workflow._resolve_base_evidence(manifest)
            self.assertEqual(set(static), {"field_validation", "visible_secondary_validation", "fault_controls"})
            self.assertEqual(temporal, {})
            seen = {}

            def reject(candidate_path, **_kwargs):
                seen["candidate"] = workflow.read_json(candidate_path)
                return {"status": "REJECTED", "errors": ["unproven static reader"]}

            with patch.object(
                    encounter_qualification, "verify_qualification", side_effect=reject) as verify:
                with self.assertRaisesRegex(workflow.WorkflowError, "unproven static reader"):
                    workflow.validate_base_evidence(
                        manifest, workflow.method_hashes(), {}, camera, {})
                verify.assert_called_once()
                self.assertEqual(
                    set(seen["candidate"]["reader"]["implementation_sha256"]),
                    set(STATIC_READER_IMPLEMENTATION_FILES))
            self.assertEqual(list(root.glob(".base-qualification-*")), [])

    def test_carried_temporal_evidence_preserves_its_classifier_inventory(self):
        helper = qualification_test_support.QualificationTests(
            "test_complete_exact_bundle_qualifies")
        helper.setUp()
        self.addCleanup(helper.tearDown)
        classifier = workflow.ARROW_CLASSIFIER_ID
        temporal, _ = helper.generic_temporal_validation(classifier)
        _static, carried = workflow._resolve_base_evidence(
            helper.write_bundle(temporal=temporal))
        self.assertEqual(
            carried[classifier]["implementation_sha256"],
            temporal[classifier]["implementation_sha256"])

    def test_failed_base_reverification_stops_before_key_consumption(self):
        from encounter_product import DEFAULT_POLICY_ID

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            base = root / "base.json"
            base.write_text("{}\n")
            campaign_root = root / "campaign"
            campaign = {"kind": workflow.CAMPAIGN_NAME, "schema_version": 1,
                        "base_manifest_sha256": workflow.sha256(base),
                        "policy_id": DEFAULT_POLICY_ID, "reader_runtime": {}, "camera": {},
                        "classifiers": {workflow.ARROW_CLASSIFIER_ID: {}}}
            workflow.write_json(campaign_root / "campaign.json", campaign)
            workflow.write_json(campaign_root / "prepared/prepared.json", {
                "kind": workflow.PREPARED_NAME,
                "campaign_sha256": workflow.sha256(campaign_root / "campaign.json")})
            with (patch.object(workflow, "_verify_frozen_source"),
                  patch.object(workflow, "_completed_observations", return_value={}),
                  patch.object(workflow, "_validate_pre_key_sources", return_value={}),
                  patch.object(workflow, "_prospective_policy", return_value=({}, b"{}", {})),
                  patch.object(workflow, "validate_base_evidence", side_effect=
                               workflow.WorkflowError("base source pixels changed")),
                  patch.object(workflow, "_consume_campaign") as consume,
                  patch.object(workflow, "_rederive_analysis") as reread):
                with self.assertRaisesRegex(workflow.WorkflowError, "base source pixels changed"):
                    workflow.finalize(campaign_root, base, root / "published.json")
                consume.assert_not_called()
                reread.assert_not_called()

    def test_arrow_reanalysis_does_not_enable_inactive_bar_or_badge(self):
        arrow = workflow.ARROW_CLASSIFIER_ID
        with tempfile.TemporaryDirectory() as directory:
            prepared = Path(directory)
            inventory = prepared / "replay-input-manifest.json"
            inventory.write_text("{}\n")
            selection_bytes = b"{}\n"
            document = {"replay_input_manifest_sha256": workflow.sha256(inventory),
                        "analysis_selection_sha256": hashlib.sha256(selection_bytes).hexdigest()}
            campaign = {"reader_runtime": {}, "implementation_sha256": {},
                        "classifiers": {arrow: {}}}

            def analyze(_run, output, _ranges, _cadence, **options):
                self.assertEqual(options["temporal_classifier_ids"], (arrow,))
                (output / "selection.json").write_bytes(selection_bytes)
                return {"errors": [], "temporal_classification": {"errors": []},
                        "evidence": {"reader": {}}, "implementation_sha256": {}}

            with (patch.object(workflow, "_verify_replay_inputs"),
                  patch.object(workflow, "reader_runtime", return_value={}),
                  patch("encounter_check.analyze", side_effect=analyze)):
                workflow._rederive_analysis(prepared, document, campaign)

    def test_prospective_policy_adds_only_the_frozen_arrow_target(self):
        arrow = workflow.ARROW_CLASSIFIER_ID
        with tempfile.TemporaryDirectory() as directory:
            policy_path = Path(directory) / "policy.json"
            workflow.write_json(policy_path, {"policies": {"selected": {
                "qualified_temporal_classifier_ids": [], "qualified_temporal_classifiers": {}}}})
            original = policy_path.read_bytes()
            campaign = {"policy_id": "selected", "classifiers": {arrow: {
                "spec": {"sha256": workflow.sha256(workflow.SPEC_BY_CLASSIFIER[arrow])}}}}
            with patch.object(workflow, "POLICY_PATH", policy_path):
                _, _, prospective = workflow._prospective_policy(campaign)
            self.assertEqual(prospective["qualified_temporal_classifier_ids"], [arrow])
            self.assertEqual(set(prospective["qualified_temporal_classifiers"]), {arrow})
            self.assertEqual(prospective["qualified_temporal_classifiers"][arrow], {
                "classifier_spec_sha256": campaign["classifiers"][arrow]["spec"]["sha256"],
                "deadline_observation_semantics": "LEGAL_PRESENTATION_TRANSITION",
                "raw_affected_fields": ["main_arrows"]})
            self.assertEqual(policy_path.read_bytes(), original)

    def test_prospective_policy_publishes_arrow_acquisition_semantics(self):
        classifier = workflow.ARROW_ACQUISITION_CLASSIFIER_ID
        with tempfile.TemporaryDirectory() as directory:
            policy_path = Path(directory) / "policy.json"
            workflow.write_json(policy_path, {"policies": {"selected": {
                "qualified_temporal_classifier_ids": [],
                "qualified_temporal_classifiers": {}}}})
            campaign = {"policy_id": "selected", "classifiers": {classifier: {
                "spec": {"sha256": workflow.sha256(
                    workflow.SPEC_BY_CLASSIFIER[classifier])}}}}
            with patch.object(workflow, "POLICY_PATH", policy_path):
                _, _, prospective = workflow._prospective_policy(campaign)
        self.assertEqual(prospective["qualified_temporal_classifier_ids"], [classifier])
        self.assertEqual(prospective["qualified_temporal_classifiers"][classifier], {
            "classifier_spec_sha256": campaign["classifiers"][classifier]["spec"]["sha256"],
            "deadline_observation_semantics": "TARGET_ACQUISITION_TRANSITION",
            "raw_affected_fields": ["main_arrows"],
        })

    def test_prospective_policy_publishes_frequency_context_semantics(self):
        classifier = workflow.FREQUENCY_CONTEXT_CLASSIFIER_ID
        with tempfile.TemporaryDirectory() as directory:
            policy_path = Path(directory) / "policy.json"
            workflow.write_json(policy_path, {"policies": {"selected": {
                "qualified_temporal_classifier_ids": [],
                "qualified_temporal_classifiers": {}}}})
            campaign = {"policy_id": "selected", "classifiers": {classifier: {
                "spec": {"sha256": workflow.sha256(
                    workflow.SPEC_BY_CLASSIFIER[classifier])}}}}
            with patch.object(workflow, "POLICY_PATH", policy_path):
                _, _, prospective = workflow._prospective_policy(campaign)
        self.assertEqual(prospective["qualified_temporal_classifier_ids"], [classifier])
        self.assertEqual(prospective["qualified_temporal_classifiers"][classifier], {
            "classifier_spec_sha256": campaign["classifiers"][classifier]["spec"]["sha256"],
            "deadline_observation_semantics": "LEGAL_PRESENTATION_TRANSITION",
            "verification_closure_semantics":
                "RAW_CURRENT_BRACKETED_UNRESOLVED_VERIFICATION_BOUNDARY",
            "raw_affected_fields": ["primary_frequency"],
        })

    def test_prospective_policy_publishes_secondary_context_semantics(self):
        classifier = workflow.SECONDARY_CONTEXT_CLASSIFIER_ID
        with tempfile.TemporaryDirectory() as directory:
            policy_path = Path(directory) / "policy.json"
            workflow.write_json(policy_path, {"policies": {"selected": {
                "qualified_temporal_classifier_ids": [],
                "qualified_temporal_classifiers": {}}}})
            campaign = {"policy_id": "selected", "classifiers": {classifier: {
                "spec": {"sha256": workflow.sha256(
                    workflow.SPEC_BY_CLASSIFIER[classifier])}}}}
            with patch.object(workflow, "POLICY_PATH", policy_path):
                _, _, prospective = workflow._prospective_policy(campaign)
        self.assertEqual(prospective["qualified_temporal_classifier_ids"], [classifier])
        self.assertEqual(prospective["qualified_temporal_classifiers"][classifier], {
            "classifier_spec_sha256": campaign["classifiers"][classifier]["spec"]["sha256"],
            "deadline_observation_semantics": "LEGAL_PRESENTATION_TRANSITION",
            "raw_affected_fields": ["secondary"],
        })

    def test_prospective_policy_publishes_secondary_optical_semantics(self):
        classifier = workflow.SECONDARY_OPTICAL_CLASSIFIER_ID
        with tempfile.TemporaryDirectory() as directory:
            policy_path = Path(directory) / "policy.json"
            workflow.write_json(policy_path, {"policies": {"selected": {
                "qualified_temporal_classifier_ids": [],
                "qualified_temporal_classifiers": {}}}})
            campaign = {"policy_id": "selected", "classifiers": {classifier: {
                "spec": {"sha256": workflow.sha256(
                    workflow.SPEC_BY_CLASSIFIER[classifier])}}}}
            with patch.object(workflow, "POLICY_PATH", policy_path):
                _, _, prospective = workflow._prospective_policy(campaign)
        self.assertEqual(prospective["qualified_temporal_classifier_ids"], [classifier])
        self.assertEqual(prospective["qualified_temporal_classifiers"][classifier], {
            "classifier_spec_sha256": campaign["classifiers"][classifier]["spec"]["sha256"],
            "deadline_observation_semantics": "LEGAL_PRESENTATION_TRANSITION",
            "raw_affected_fields": ["secondary"],
        })

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
        for classifier in ("v1-main-bar-adjacent-redraw-v2", workflow.ARROW_CLASSIFIER_ID):
            with self.subTest(classifier=classifier):
                self._lossless_media_round_trip(classifier)

    def _lossless_media_round_trip(self, classifier):
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
            source_rows = {
                index: {"source_frame_seq": index + 1,
                        "capture_ns": 1_000_000_000 + index * 5_000_000}
                for index in range(12)
            }
            source_indices, size = workflow._build_clip(
                video, clip, classifier, [4, 5, 6], source_rows,
                registration, 1280, 720)
            item = {
                "opaque_id": "OPAQUE123456", "clip": "clips/OPAQUE123456.mov",
                "sha256": workflow.sha256(clip), "size_bytes": size,
                "target_run_video_indices": [4, 5, 6],
                "clip_source_video_indices": source_indices,
                "target_run_clip_frame_indices": [source_indices.index(value)
                                                   for value in (4, 5, 6)],
                "inset_source_box": list(workflow._raw_box(
                    workflow._logical_inset(classifier),
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
                classifier, paths,
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
                    classifier, paths,
                    {"video_timing_verification": timing},
                    {"camera": {"capture_id": capture_manifest["capture_id"],
                                "video_timing_verification_result": timing}},
                    selection, [item])

    def test_build_clip_retains_fixed_time_context_and_slows_for_review(self):
        with tempfile.TemporaryDirectory() as temporary:
            destination = Path(temporary) / "clip.mov"
            commands = []

            def run(command, **_kwargs):
                commands.append(command)
                Path(command[-1]).write_bytes(b"observer clip")

            probe = {"streams": [{
                "codec_name": "png", "pix_fmt": "rgb24", "width": 1720, "height": 720,
                "nb_frames": "100", "r_frame_rate": "25/1",
            }]}
            source_rows = {
                index: {"source_frame_seq": index + 1,
                        "capture_ns": 1_000_000_000 + index * 5_000_000}
                for index in range(100)
            }
            with (patch.object(workflow.shutil, "which", side_effect=lambda name: f"/{name}"),
                  patch.object(workflow.subprocess, "run", side_effect=run),
                  patch.object(workflow.subprocess, "check_output",
                               return_value=json.dumps(probe))):
                source, size = workflow._build_clip(
                    Path("reserved.mp4"), destination,
                    "v1-main-bar-adjacent-redraw-v2", [50, 51, 52], source_rows,
                    {"landmark_bounds": [376, 192, 595, 270]}, 1280, 720)

            self.assertEqual(source, list(range(100)))
            self.assertEqual(size, len(b"observer clip"))
            graph = commands[0][commands[0].index("-filter_complex") + 1]
            self.assertIn("trim=start_frame=0:end_frame=100", graph)
            self.assertIn("setpts=N/(25*TB)", graph)

    def test_observer_window_is_decision_and_full_run_independent(self):
        source_rows = {
            index: {"source_frame_seq": index + 1,
                    "capture_ns": 1_000_000_000 + index * 1_000_000}
            for index in range(801)
        }
        candidates = (
            {"decision": "ADMITTED", "indices": [400, 401],
             "full_indices": list(range(100, 702))},
            {"decision": "REJECTED", "indices": [400, 401],
             "full_indices": [400, 401]},
        )
        windows = [workflow._observer_clip_source_indices(
            candidate["indices"], source_rows) for candidate in candidates]
        self.assertEqual(windows[0], windows[1])
        self.assertEqual(windows[0], list(range(100, 702)))
        self.assertGreater(len(windows[0]), 40)

    def test_prepare_classifier_output_passes_the_real_v2_verifier(self):
        self._prepared_classifier_round_trip("v1-main-bar-adjacent-redraw-v2")

    def test_prepare_arrow_packet_passes_the_real_verifier(self):
        self._prepared_classifier_round_trip(workflow.ARROW_CLASSIFIER_ID)

    def test_prepare_arrow_acquisition_packet_passes_the_real_verifier(self):
        self._prepared_classifier_round_trip(workflow.ARROW_ACQUISITION_CLASSIFIER_ID)

    def test_prepare_frequency_context_packet_passes_the_real_verifier(self):
        self._prepared_classifier_round_trip(workflow.FREQUENCY_CONTEXT_CLASSIFIER_ID)

    def test_prepare_secondary_context_packet_passes_the_real_verifier(self):
        self._prepared_classifier_round_trip(workflow.SECONDARY_CONTEXT_CLASSIFIER_ID)

    def test_prepare_secondary_optical_packet_passes_the_real_verifier(self):
        self._prepared_classifier_round_trip(workflow.SECONDARY_OPTICAL_CLASSIFIER_ID)

    def _prepared_classifier_round_trip(self, classifier):
        helper = qualification_test_support.QualificationTests(
            "test_complete_exact_bundle_qualifies")
        helper.setUp()
        self.addCleanup(helper.tearDown)
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

        def build_clip(_video, destination, _classifier, target, source_rows,
                       _registration, _width, _height):
            source_indices = workflow._observer_clip_source_indices(target, source_rows)
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(f"workflow clip {destination.stem}".encode("ascii"))
            return source_indices, destination.stat().st_size

        fake_data = {
            "video": Path("reserved.mp4"),
            "retained_capture": helper.root / "retained-capture",
            "timing": {"encoded_frame_count": 1000},
            "rows": [{"frame_seq": 1000 + index,
                      "host_capture_ns": 1_000_000_000 + index * 5_000_000}
                     for index in range(1000)],
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
        readme = workflow_sources["observer_readme"].read_text(encoding="utf-8")
        self.assertNotIn("full_run", readme)
        hidden_by_id = {item["opaque_id"]: item for item in prepared_hidden["items"]}
        for item in manifest["items"]:
            self.assertNotIn("full_run_video_indices", item)
            self.assertNotIn("full_run_clip_frame_indices", item)
            hidden_item = hidden_by_id[item["opaque_id"]]
            self.assertIn("full_run_video_indices", hidden_item)
            self.assertIn("full_run_clip_frame_indices", hidden_item)
            self.assertEqual(
                hidden_item["full_run_clip_frame_indices"],
                [item["clip_source_video_indices"].index(value)
                 for value in hidden_item["full_run_video_indices"]],
            )
        completed = workflow.read_json(workflow_sources["completed_observations"])
        comparison = workflow._matrix_document(
            classifier, seeded_entry["classifier_spec_sha256"], workflow_sources,
            manifest, completed, prepared_hidden)
        comparison_path = classifier_root / "comparison.json"
        workflow.write_json(comparison_path, comparison)
        entry = {
            "classifier_spec_sha256": seeded_entry["classifier_spec_sha256"],
            "implementation_sha256": implementation,
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
                expected_rejections = (
                    10 if classifier == workflow.FREQUENCY_CONTEXT_CLASSIFIER_ID else 5)
                self.assertEqual(document["confusion_matrix"]["true_reject"], 0)
                self.assertEqual(document["confusion_matrix"]["abstain"], expected_rejections)
                self.assertEqual(
                    document["denominators"]["observer_indeterminate"], expected_rejections)
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

    def _static_reanalysis_fixture(self, destination, *, observation_mutator=None,
                                   helper=None, source_manifest=None,
                                   method_version=6, reader_sha="e" * 64):
        if helper is None:
            helper = qualification_test_support.QualificationTests(
                "test_complete_exact_bundle_qualifies")
            helper.setUp()
            self.addCleanup(helper.tearDown)
        if source_manifest is None:
            source_manifest = helper.write_bundle()
        current_method = dict(helper.method)
        current_method["encounter_reader.py"] = reader_sha
        current_runtime = dict(qualification_test_support.READER)
        current_runtime["method_version"] = method_version
        session_state = {"active": False, "entries": 0, "verified": False}

        @contextmanager
        def analysis_session():
            self.assertFalse(session_state["active"])
            session_state["active"] = True
            session_state["entries"] += 1
            try:
                yield
            finally:
                session_state["active"] = False

        def regenerated(image_path, _registration):
            self.assertTrue(session_state["active"])
            observation = json.loads(json.dumps(
                helper.reader_observations[qualification_test_support.digest(image_path)]))
            return observation_mutator(observation) if observation_mutator else observation

        real_verify = encounter_qualification.verify_qualification

        def verify(*args, **kwargs):
            self.assertTrue(session_state["active"])
            session_state["verified"] = True
            return real_verify(*args, **kwargs)

        empty_policy = {
            "contract_version": 3,
            "qualified_temporal_classifier_ids": [],
            "qualified_temporal_classifiers": {},
        }
        patches = (
            patch.object(workflow, "git_identity", return_value=("f" * 40, True)),
            patch.object(workflow, "method_hashes", return_value=current_method),
            patch.object(workflow, "reader_runtime", return_value=current_runtime),
            patch.object(encounter_qualification, "_observe_image", side_effect=regenerated),
            patch("encounter_product.load_policy", return_value=empty_policy),
            patch("encounter_reader.analysis_session", side_effect=analysis_session),
            patch.object(encounter_qualification, "verify_qualification", side_effect=verify),
        )
        return (helper, source_manifest, current_method, current_runtime, patches,
                session_state)

    def test_static_reanalysis_publishes_only_verified_current_bundle(self):
        with tempfile.TemporaryDirectory() as temporary:
            destination = Path(temporary) / "static-v6"
            helper, source, method, runtime, patches, session = (
                self._static_reanalysis_fixture(destination))
            source_bytes = {path.relative_to(helper.root): path.read_bytes()
                            for path in helper.root.rglob("*") if path.is_file()}
            with ExitStack() as stack:
                for item in patches:
                    stack.enter_context(item)
                result = workflow.reanalyze_static(source, destination)

            self.assertEqual(result["status"], "QUALIFIED")
            self.assertEqual(session,
                             {"active": False, "entries": 1, "verified": True})
            manifest_path = destination / "encounter-reader.json"
            self.assertTrue(manifest_path.is_file())
            manifest = workflow.read_json(manifest_path)
            self.assertEqual(manifest["reader"]["method_version"], 6)
            self.assertEqual(
                manifest["reader"]["implementation_sha256"],
                workflow.static_method_hashes(method))
            self.assertEqual(manifest["temporal_classifiers"], {})
            secondary_path = workflow.resolve_reference(
                destination, manifest["visible_secondary_validation"], "secondary")
            secondary = workflow.read_json(secondary_path)
            self.assertEqual(secondary["reader_reanalysis"], {
                "kind": "complete_exact_reader_reread",
                "source_sealed_key_sha256":
                    secondary["source_artifacts"]["sealed_key"]["sha256"],
                "source_method_version": 5,
                "source_reader_sha256": "a" * 64,
                "current_method_version": 6,
                "current_reader_sha256": "e" * 64,
                "complete_source_set_reread": True,
            })
            diagnostic = workflow.read_json(destination / "reanalysis-result.json")
            self.assertEqual(diagnostic["verification"]["status"], "QUALIFIED")
            self.assertEqual(
                diagnostic["verification"]["visible_secondary_validation"][
                    "partial_secondary_identity_agreement_frames"], 6)
            self.assertEqual(
                {path.relative_to(helper.root): path.read_bytes()
                 for path in helper.root.rglob("*") if path.is_file()},
                source_bytes)

    def test_static_reanalysis_retains_rejection_without_publishing_manifest(self):
        def wrong_partial_identity(observation):
            secondary = observation.get("secondary")
            partial = secondary.get("partial_cards") if isinstance(secondary, dict) else None
            if isinstance(partial, list) and partial and partial[0].get("band") is not None:
                partial[0]["band"] = "K"
                partial[0]["frequency"] = "24.150"
            return observation

        with tempfile.TemporaryDirectory() as temporary:
            destination = Path(temporary) / "rejected-static-v6"
            _helper, source, _method, _runtime, patches, session = (
                self._static_reanalysis_fixture(
                    destination, observation_mutator=wrong_partial_identity))
            with ExitStack() as stack:
                for item in patches:
                    stack.enter_context(item)
                with self.assertRaisesRegex(workflow.WorkflowError,
                                            "diagnostics retained"):
                    workflow.reanalyze_static(source, destination)
            self.assertEqual(session,
                             {"active": False, "entries": 1, "verified": True})
            self.assertTrue(destination.is_dir())
            self.assertFalse((destination / "encounter-reader.json").exists())
            self.assertTrue((destination / "rejected-candidate.json").is_file())
            diagnostic = workflow.read_json(destination / "reanalysis-result.json")
            self.assertEqual(diagnostic["status"], "REJECTED")
            self.assertIn("wrong partial identity",
                          diagnostic["verification"]["errors"][0])

    def test_static_reanalysis_refuses_publish_if_reader_changes_during_reread(self):
        with tempfile.TemporaryDirectory() as temporary:
            destination = Path(temporary) / "changed-static-v6"
            _helper, source, method, _runtime, patches, session = (
                self._static_reanalysis_fixture(destination))
            changed = dict(method)
            changed["encounter_reader.py"] = "d" * 64
            with ExitStack() as stack:
                for index, item in enumerate(patches):
                    if index != 1:
                        stack.enter_context(item)
                stack.enter_context(patch.object(
                    workflow, "method_hashes", side_effect=[method, changed]))
                with self.assertRaisesRegex(workflow.WorkflowError,
                                            "implementation changed during"):
                    workflow.reanalyze_static(source, destination)
            self.assertEqual(session,
                             {"active": False, "entries": 1, "verified": True})
            self.assertFalse((destination / "encounter-reader.json").exists())
            diagnostic = workflow.read_json(destination / "reanalysis-result.json")
            self.assertEqual(diagnostic["status"], "ERROR")

    def test_static_reanalysis_can_reuse_a_prior_current_reader_reread(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            first = root / "static-v6"
            helper, source, _method, _runtime, patches, _session = (
                self._static_reanalysis_fixture(first))
            with ExitStack() as stack:
                for item in patches:
                    stack.enter_context(item)
                workflow.reanalyze_static(source, first)

            first_manifest = first / "encounter-reader.json"
            second = root / "static-v7"
            (_helper, chained_source, _method, _runtime, patches, session) = (
                self._static_reanalysis_fixture(
                    second, helper=helper, source_manifest=first_manifest,
                    method_version=7, reader_sha="d" * 64))
            with ExitStack() as stack:
                for item in patches:
                    stack.enter_context(item)
                result = workflow.reanalyze_static(chained_source, second)

            self.assertEqual(result["status"], "QUALIFIED")
            self.assertEqual(session,
                             {"active": False, "entries": 1, "verified": True})
            manifest = workflow.read_json(second / "encounter-reader.json")
            secondary = workflow.read_json(workflow.resolve_reference(
                second, manifest["visible_secondary_validation"], "secondary"))
            self.assertEqual(secondary["reader_reanalysis"]["source_method_version"], 5)
            self.assertEqual(secondary["reader_reanalysis"]["source_reader_sha256"], "a" * 64)
            self.assertEqual(secondary["reader_reanalysis"]["current_method_version"], 7)
            self.assertEqual(secondary["reader_reanalysis"]["current_reader_sha256"], "d" * 64)

    def test_static_reanalysis_cli_is_explicit_and_bounded(self):
        args = workflow.build_parser().parse_args([
            "reanalyze-static", "--source-manifest", "old.json", "--out", "new-static"])
        self.assertEqual(args.command, "reanalyze-static")
        self.assertEqual(args.source_manifest, Path("old.json"))
        self.assertEqual(args.out, Path("new-static"))

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
