#!/usr/bin/env python3
"""Regression tests for static reader requalification."""

from __future__ import annotations

import json
import io
import hashlib
import os
from contextlib import ExitStack, contextmanager
from pathlib import Path
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
    COMMON_TEMPORAL_IMPLEMENTATION_FILES,
    STATIC_READER_IMPLEMENTATION_FILES,
)
import test_encounter_qualification as qualification_test_support

class QualificationWorkflowTests(unittest.TestCase):
    def test_removed_deadline_campaign_commands_are_not_available(self):
        for command in ("freeze", "prepare", "finalize"):
            with self.subTest(command=command), patch.object(sys, "stderr", io.StringIO()):
                with self.assertRaises(SystemExit) as error:
                    workflow.build_parser().parse_args([command])
                self.assertEqual(error.exception.code, 2)



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

        patches = (
            patch.object(workflow, "git_identity", return_value=("f" * 40, True)),
            patch.object(workflow, "method_hashes", return_value=current_method),
            patch.object(workflow, "reader_runtime", return_value=current_runtime),
            patch.object(encounter_qualification, "_observe_image", side_effect=regenerated),
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

    def test_explicit_frequency_supplement_preserves_colliding_historical_names(self):
        helper = qualification_test_support.QualificationTests("test_complete_exact_bundle_qualifies")
        helper.setUp()
        self.addCleanup(helper.tearDown)
        source = helper.root / "field.json"
        document = helper.field_validation()

        def packet(directory, label):
            directory.mkdir(parents=True)
            refs = {}
            for name in ("protocol", "blind_manifest", "observations"):
                artifact = directory / (name + ".json")
                artifact.write_text(json.dumps({"label": label}))
                refs[name] = workflow.reference(artifact, directory)
            path = directory / "reference.json"
            workflow.write_json(path, {**refs, "items": []})
            return path

        old = packet(helper.root / "source/primary-frequency", "historical")
        new = packet(helper.root / "supplement", "new independent labels")
        document["source_artifacts"]["primary_frequency_reference"] = workflow.reference(old, helper.root)
        workflow.write_json(source, document)
        original = {p: p.read_bytes() for p in old.parent.iterdir()}
        first = helper.root / "first/field.json"
        second = helper.root / "second/field.json"
        with patch("encounter_primary_frequency_reference.reference_reread_binding", return_value=None), \
             patch("encounter_primary_frequency_reference.validate_reference", return_value={"summary": {}, "overrides": {}}), \
             patch.object(encounter_qualification, "_observe_image", side_effect=lambda p, _r: helper.reader_observations[qualification_test_support.digest(p)]):
            result = workflow._reanalyze_field_document(
                source, first, qualification_test_support.READER, helper.method,
                qualification_test_support.CAMERA, new)
            repeated = workflow._reanalyze_field_document(
                first, second, qualification_test_support.READER, helper.method,
                qualification_test_support.CAMERA, new)
        for output, current in ((first, result), (second, repeated)):
            selected = workflow.resolve_reference(output.parent, current["source_artifacts"]["primary_frequency_reference"], "supplement")
            self.assertEqual(selected.read_bytes(), new.read_bytes())
            self.assertEqual((selected.parent / "observations.json").read_bytes(), (new.parent / "observations.json").read_bytes())
        self.assertEqual((first.parent / "source/primary-frequency/observations.json").read_bytes(), original[old.parent / "observations.json"])
        self.assertEqual({p: p.read_bytes() for p in old.parent.iterdir()}, original)

    def test_static_reanalysis_rebinds_changed_qualification_with_same_reader(self):
        with tempfile.TemporaryDirectory() as temporary:
            destination = Path(temporary) / "static-same-reader"
            helper, source, method, _runtime, patches, session = (
                self._static_reanalysis_fixture(
                    destination, method_version=5, reader_sha="a" * 64))
            method["encounter_qualification.py"] = "f" * 64
            with ExitStack() as stack:
                for item in patches:
                    stack.enter_context(item)
                result = workflow.reanalyze_static(source, destination)

            self.assertEqual(result["status"], "QUALIFIED")
            self.assertEqual(session,
                             {"active": False, "entries": 1, "verified": True})
            manifest = workflow.read_json(destination / "encounter-reader.json")
            self.assertEqual(
                manifest["reader"]["implementation_sha256"]["encounter_qualification.py"],
                "f" * 64)
            secondary = workflow.read_json(workflow.resolve_reference(
                destination, manifest["visible_secondary_validation"], "secondary"))
            self.assertNotIn("reader_reanalysis", secondary)
            diagnostic = workflow.read_json(destination / "reanalysis-result.json")
            self.assertFalse(
                diagnostic["verification"]["visible_secondary_validation"][
                    "reader_reanalysis"])

    def test_static_reanalysis_does_not_hide_unversioned_reader_helper_change(self):
        with tempfile.TemporaryDirectory() as temporary:
            destination = Path(temporary) / "static-helper-drift"
            _helper, source, method, _runtime, patches, _session = (
                self._static_reanalysis_fixture(
                    destination, method_version=5, reader_sha="a" * 64))
            method["encounter_ocr_session.py"] = "f" * 64
            with ExitStack() as stack:
                for item in patches:
                    stack.enter_context(item)
                with self.assertRaisesRegex(workflow.WorkflowError,
                                            "diagnostics retained"):
                    workflow.reanalyze_static(source, destination)

            candidate = workflow.read_json(destination / "rejected-candidate.json")
            secondary = workflow.read_json(workflow.resolve_reference(
                destination, candidate["visible_secondary_validation"], "secondary"))
            self.assertIn("reader_reanalysis", secondary)
            diagnostic = workflow.read_json(destination / "reanalysis-result.json")
            self.assertIn("does not identify a different reader",
                          diagnostic["verification"]["errors"][0])

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

    def test_static_reanalysis_records_stable_uncommitted_method_truthfully(self):
        with tempfile.TemporaryDirectory() as temporary:
            destination = Path(temporary) / "pending-method"
            _, source, method, _, patches, _ = self._static_reanalysis_fixture(destination)
            with ExitStack() as stack:
                for index, item in enumerate(patches):
                    if index != 0:
                        stack.enter_context(item)
                stack.enter_context(patch.object(workflow, "git_identity", return_value=("f" * 40, False)))
                workflow.reanalyze_static(source, destination)
            manifest = workflow.read_json(destination / "encounter-reader.json")
            diagnostic = workflow.read_json(destination / "reanalysis-result.json")
            self.assertFalse(manifest["source"]["worktree_clean"])
            self.assertFalse(diagnostic["source_worktree_clean"])
            self.assertEqual(manifest["source"]["static_method_sha256"],
                             hashlib.sha256(workflow.json_bytes(workflow.static_method_hashes(method))).hexdigest())
            self.assertIn(manifest["source"]["static_method_sha256"][:12], manifest["qualification_id"])

    def test_dirty_static_method_still_rejects_changed_reader_bytes(self):
        with tempfile.TemporaryDirectory() as temporary:
            destination = Path(temporary) / "changed-pending-method"
            _, source, method, _, patches, _ = self._static_reanalysis_fixture(destination)
            changed = {**method, "encounter_reader.py": "d" * 64}
            with ExitStack() as stack:
                for index, item in enumerate(patches):
                    if index not in (0, 1):
                        stack.enter_context(item)
                stack.enter_context(patch.object(workflow, "git_identity", return_value=("f" * 40, False)))
                stack.enter_context(patch.object(workflow, "method_hashes", side_effect=[method, changed]))
                with self.assertRaisesRegex(workflow.WorkflowError, "implementation changed during"):
                    workflow.reanalyze_static(source, destination)
            self.assertFalse((destination / "encounter-reader.json").exists())

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
                return Namespace(command="reanalyze-static", source_manifest=Path("source"),
                                 out=Path("output"), primary_frequency_reference=None, secondary_reference=None)

        error = encounter_qualification.QualificationError("capture boundary rejected")
        stderr = io.StringIO()
        with (patch.object(workflow, "build_parser", return_value=Parser()),
              patch.object(workflow, "reanalyze_static", side_effect=error),
              patch.object(sys, "stderr", stderr)):
            self.assertEqual(workflow.main(), 2)
        self.assertEqual(
            stderr.getvalue(),
            "qualification workflow failed: capture boundary rejected\n",
        )

if __name__ == "__main__":
    unittest.main()
