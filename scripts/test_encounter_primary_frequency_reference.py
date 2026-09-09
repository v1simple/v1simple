#!/usr/bin/env python3
"""Independent reference corrections preserve history and reject false admissions."""
from contextlib import ExitStack
from copy import deepcopy
import errno
import hashlib
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).parent))
sys.path.insert(0, str(Path(__file__).parent / "bench"))
from encounter_primary_frequency_reference import (BLIND_PROTOCOL, CONTROL_STATES, copy_reference,
                                                    reference_reread_binding, validate_reference)
import encounter_primary_frequency_reference as frequency_reference
from encounter_qualification import CORE_READER_FILES, LEGACY_CORE_READER_FILES


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value))
    return path


def fixture(root, original_manifest=None, original_observations=None, original_image=None,
            source_id="old-1", method=None, reader_observations=None):
    root.mkdir(parents=True, exist_ok=True)
    method = method or {name: "a" * 64 for name in CORE_READER_FILES}
    readings = {} if reader_observations is None else reader_observations
    if original_image is None:
        original_image = root / "original.png"
        original_image.write_bytes(b"unchanged original camera pixels")
    original_manifest = original_manifest or write(root / "old-manifest.json", {
        "frames": [{"frame_id": source_id, "image_sha256": sha(original_image)}]})
    original_observations = original_observations or write(root / "old-observations.json", {
        "frames": [{"frame_id": source_id, "primary_frequency": {"state": "absent", "value": None}}]})
    packet = root / "packet"
    packet.mkdir()
    def ref(path):
        return {"path": str(path.relative_to(packet)), "sha256": sha(path)}
    items, frames, labels = [], [], []
    for i, role in enumerate(("reference_correction", "held_out_original", "held_out_original", *CONTROL_STATES)):
        frame_id = f"opaque-{i}"
        path = packet / f"images/{frame_id}.png"
        path.parent.mkdir(exist_ok=True)
        path.write_bytes(original_image.read_bytes() if i == 0 else ("fixed source image " + frame_id).encode())
        state = "readable" if i < 3 else "absent" if role == "missing_region_control" else "ambiguous"
        label = {"state": state, "value": "--.---" if state == "readable" else None,
                 "reason": "Independent literal image observation"}
        image = ref(path)
        frames.append({"frame_id": frame_id, "image": image})
        labels.append({"frame_id": frame_id, "image_sha256": image["sha256"], "primary_frequency": label})
        item = {"frame_id": frame_id, "image": image, "role": role}
        if i == 0:
            item["source_frame_id"] = source_id
        elif i < 3:
            item.update(not_used_for_reader_development=True,
                        origin={"capture_id": "independent-capture", "video_frame_index": i, "video_sha256": "b" * 64})
        else:
            item["operation"] = {"declared_pixel_change": role}
        items.append(item)
        readings.setdefault(image["sha256"], {})["primary_frequency"] = deepcopy(label)
    protocol = packet / "protocol.md"
    protocol.write_text("Read only opaque original images. No source, expectations, prior labels or reader outputs.")
    manifest = write(packet / "blind-manifest.json", {"schema_version": 1, "frames": frames})
    observations = write(packet / "observations.json", {"schema_version": 1,
        "kind": "blind_primary_frequency_observations", "blind_protocol": dict(BLIND_PROTOCOL), "frames": labels})
    document = {"schema_version": 1, "kind": "independent_primary_frequency_reference",
                "original_blind_manifest_sha256": sha(original_manifest),
                "original_blind_observations_sha256": sha(original_observations),
                "frozen_reader_files": {name: method[name] for name in CORE_READER_FILES},
                "protocol": ref(protocol), "blind_manifest": ref(manifest), "observations": ref(observations),
                "items": items}
    path = write(packet / "reference.json", document)
    args = [path, original_manifest, original_observations, method, {"result": "PASS"},
            lambda image, registration: deepcopy(readings[sha(image)])]
    return args, document, readings


class EvidenceCopyTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.source = self.root / "original.png"
        self.source.write_bytes(b"retained original pixels")
        self.expected = sha(self.source)
        self.destination = self.root / "copy" / "original.png"

    def test_portable_fallback_retains_separate_files_and_metadata(self):
        os.utime(self.source, ns=(1_000_000_000, 2_000_000_000))
        with patch.object(frequency_reference.sys, "platform", "unsupported"):
            frequency_reference.copy_evidence(self.source, self.destination, self.expected)
        self.assertEqual(sha(self.destination), self.expected)
        self.assertEqual(self.destination.stat().st_mtime_ns, self.source.stat().st_mtime_ns)
        self.assertFalse(self.source.samefile(self.destination))
        self.destination.write_bytes(b"later destination edit")
        self.assertEqual(sha(self.source), self.expected)
        self.source.write_bytes(b"later source edit")
        self.assertEqual(self.destination.read_bytes(), b"later destination edit")

    def test_identical_destination_is_reused_without_rewriting(self):
        self.destination.parent.mkdir()
        self.destination.write_bytes(self.source.read_bytes())
        os.utime(self.destination, ns=(3_000_000_000, 4_000_000_000))
        before = self.destination.stat()
        with patch.object(frequency_reference, "_clone_file") as clone, \
                patch.object(frequency_reference.shutil, "copy2") as copy:
            frequency_reference.copy_evidence(self.source, self.destination, self.expected)
        clone.assert_not_called()
        copy.assert_not_called()
        after = self.destination.stat()
        self.assertEqual((after.st_ino, after.st_mtime_ns, after.st_ctime_ns),
                         (before.st_ino, before.st_mtime_ns, before.st_ctime_ns))

    def test_mismatched_source_or_destination_is_refused_without_overwriting(self):
        with self.assertRaisesRegex(ValueError, "changed copy source"):
            frequency_reference.copy_evidence(self.source, self.destination, "0" * 64)
        self.assertFalse(self.destination.exists())
        self.destination.parent.mkdir()
        self.destination.write_bytes(b"different retained evidence")
        with self.assertRaisesRegex(ValueError, "path collision"):
            frequency_reference.copy_evidence(self.source, self.destination, self.expected)
        self.assertEqual(self.destination.read_bytes(), b"different retained evidence")
        self.assertEqual(sha(self.source), self.expected)

    def test_symbolic_link_sources_and_destinations_are_refused(self):
        source_link = self.root / "source-link.png"
        source_link.symlink_to(self.source)
        with self.assertRaisesRegex(ValueError, "indirect"):
            frequency_reference.copy_evidence(source_link, self.destination, self.expected)
        self.destination.parent.mkdir()
        for target in (self.source, self.root / "missing.png"):
            with self.subTest(target=target.name):
                self.destination.symlink_to(target)
                with self.assertRaisesRegex(ValueError, "symbolic link"):
                    frequency_reference.copy_evidence(self.source, self.destination, self.expected)
                self.assertTrue(self.destination.is_symlink())
                self.destination.unlink()

    def test_changed_copy_bytes_are_rejected(self):
        def bad_copy(source, destination):
            destination.write_bytes(b"changed bytes")
        with patch.object(frequency_reference, "_clone_file", return_value=False), \
                patch.object(frequency_reference.shutil, "copy2", side_effect=bad_copy):
            with self.assertRaisesRegex(ValueError, "changed while copying"):
                frequency_reference.copy_evidence(self.source, self.destination, self.expected)
        self.assertEqual(sha(self.source), self.expected)

    def test_native_fallback_is_limited_to_unsupported_clone_operations(self):
        for error in (errno.ENOTSUP, errno.ENOSYS, errno.EXDEV, errno.EACCES,
                      errno.EPERM, errno.ENOSPC, errno.EIO):
            with self.subTest(error=error), \
                    patch.object(frequency_reference.sys, "platform", "darwin"), \
                    patch.object(frequency_reference.ctypes, "CDLL") as library, \
                    patch.object(frequency_reference.ctypes, "get_errno", return_value=error), \
                    patch.object(frequency_reference.shutil, "copy2") as copy:
                library.return_value.clonefile.return_value = -1
                if error in (errno.ENOTSUP, errno.ENOSYS, errno.EXDEV):
                    copy.side_effect = lambda source, dest: dest.write_bytes(source.read_bytes())
                    frequency_reference.copy_evidence(self.source, self.destination, self.expected)
                    copy.assert_called_once_with(self.source, self.destination)
                    self.destination.unlink()
                else:
                    with self.assertRaises(OSError) as failure:
                        frequency_reference.copy_evidence(self.source, self.destination, self.expected)
                    self.assertEqual(failure.exception.errno, error)
                    copy.assert_not_called()
                    self.assertFalse(self.destination.exists())

    @unittest.skipUnless(sys.platform == "darwin", "native clonefile requires macOS")
    def test_native_clone_retains_independent_bytes(self):
        self.destination.parent.mkdir()
        if not frequency_reference._clone_file(self.source, self.destination):
            self.skipTest("temporary filesystem does not support clonefile")
        self.assertEqual(sha(self.destination), self.expected)
        self.assertFalse(self.source.samefile(self.destination))
        self.destination.write_bytes(b"changed clone")
        self.assertEqual(sha(self.source), self.expected)
        self.source.write_bytes(b"changed original")
        self.assertEqual(self.destination.read_bytes(), b"changed clone")


class PrimaryFrequencyReferenceTests(unittest.TestCase):
    def test_version_24_cannot_qualify_from_old_only_references(self):
        import encounter_qualification_workflow as workflow
        from test_encounter_qualification_workflow import QualificationWorkflowTests
        support = QualificationWorkflowTests("test_static_reanalysis_publishes_only_verified_current_bundle")
        self.addCleanup(support.doCleanups)
        with tempfile.TemporaryDirectory() as temp:
            destination = Path(temp) / "qualified"
            _, source, _, _, patches, _ = support._static_reanalysis_fixture(destination, method_version=24)
            with ExitStack() as stack:
                for item in patches:
                    stack.enter_context(item)
                with self.assertRaisesRegex(workflow.WorkflowError, "static reanalysis rejected"):
                    workflow.reanalyze_static(source, destination)
            diagnostic = workflow.read_json(destination / "reanalysis-result.json")
            self.assertTrue(any("two qualified startup" in error
                                for error in diagnostic["verification"]["errors"]), diagnostic)
            self.assertFalse((destination / "encounter-reader.json").exists())

    def test_version_24_branch_coverage_is_derived_from_exact_reread_not_packet_claims(self):
        import encounter_qualification_workflow as workflow
        from test_encounter_qualification_workflow import QualificationWorkflowTests
        for case in ("complete", "one_source", "nine_acceptances", "no_residual_refusal"):
            with self.subTest(case=case), tempfile.TemporaryDirectory() as temp:
                support = QualificationWorkflowTests("test_static_reanalysis_publishes_only_verified_current_bundle")
                self.addCleanup(support.doCleanups)
                destination = Path(temp) / "qualified"
                helper, source, method, _, patches, _ = support._static_reanalysis_fixture(destination, method_version=24)
                field_path = workflow.resolve_reference(source.parent, workflow.read_json(source)["field_validation"], "field")
                field = workflow.read_json(field_path)
                original_manifest = workflow.resolve_reference(field_path.parent, field["source_artifacts"]["blind_manifest"], "manifest")
                original_labels = workflow.resolve_reference(field_path.parent, field["source_artifacts"]["blind_observations"], "labels")
                original_image = workflow.resolve_reference(field_path.parent, field["frames"][0]["image"], "image")
                args, document, readings = fixture(Path(temp) / "supplement", original_manifest, original_labels,
                    original_image, field["frames"][0]["frame_id"], method, helper.reader_observations)
                registration, _, _ = self.startup_input(args, document)
                first_startup = deepcopy(document["items"][1]["startup_calibration"])
                packet = args[0].parent
                second_still = packet / "startup/second.jpg"
                second_still.write_bytes(b"another original startup image")
                second_preflight = write(packet / "startup/second-preflight.json", {
                    "result": "PASS", "registration": registration,
                    "source_still": {"name": second_still.name, "sha256": sha(second_still)}})
                second_startup = {
                    "preflight": {"path": "startup/second-preflight.json", "sha256": sha(second_preflight)},
                    "still": {"path": "startup/second.jpg", "sha256": sha(second_still)}}
                accepted = {"state": "readable", "value": "--.---", "reason": "Independent literal",
                            "calibrated_idle": {"calibration_qualified": True, "accepted": True}}
                for item in document["items"][1:3]:
                    readings[item["image"]["sha256"]]["primary_frequency"] = deepcopy(accepted)
                for index in range(8):
                    item = RetainedFrequencySupplementTests.append(args, document, readings, "retained_original",
                        {"state": "readable", "value": "--.---", "reason": "Independent literal"}, accepted)
                    item["registration"] = deepcopy(registration)
                    item["startup_calibration"] = deepcopy(first_startup if case == "one_source" else second_startup)
                if case == "nine_acceptances":
                    # A readable old-path result cannot count as use of the new branch.
                    readings[document["items"][-1]["image"]["sha256"]]["primary_frequency"].pop("calibrated_idle")
                refused = {"state": "ambiguous", "value": None, "reason": "Visible residual ink",
                           "calibrated_idle": {"calibration_qualified": True, "accepted": False,
                                               "residual_ink": {"clear": False}}}
                if case == "no_residual_refusal":
                    refused["calibrated_idle"] = {"calibration_qualified": True, "accepted": False,
                                                   "template_match": False}
                item = RetainedFrequencySupplementTests.append(args, document, readings, "extra_ink_control",
                    {"state": "ambiguous", "value": None, "reason": "Independent extra ink observation"}, refused)
                item["registration"] = deepcopy(registration)
                item["startup_calibration"] = deepcopy(first_startup)
                # A packet-authored success claim is deliberately not authoritative.
                document["calibrated_idle_coverage"] = {"qualified": True,
                    "qualified_startup_image_sha256": ["a" * 64, "b" * 64],
                    "calibrated_idle_acceptances": 100, "residual_ink_refusals": 100}
                write(args[0], document)
                def calibrate(preflight, path):
                    return {**preflight["registration"], "primary_frequency_calibration": {"qualified": True}}
                with ExitStack() as stack:
                    for item in patches:
                        stack.enter_context(item)
                    stack.enter_context(patch.dict(sys.modules, {
                        "encounter_frequency_idle": SimpleNamespace(registration_for_camera=calibrate)}))
                    if case == "complete":
                        result = workflow.reanalyze_static(source, destination, args[0])
                        self.assertEqual(result["status"], "QUALIFIED")
                    else:
                        with self.assertRaisesRegex(workflow.WorkflowError, "static reanalysis rejected"):
                            workflow.reanalyze_static(source, destination, args[0])
                diagnostic = workflow.read_json(destination / "reanalysis-result.json")
                if case == "complete":
                    coverage = diagnostic["verification"]["field_validation"]["primary_frequency_adjudication"]["calibrated_idle_coverage"]
                    self.assertEqual(len(coverage["qualified_startup_image_sha256"]), 2)
                    self.assertEqual(coverage["calibrated_idle_acceptances"], 10)
                    self.assertEqual(coverage["residual_ink_refusals"], 1)
                else:
                    expected = {"one_source": "two qualified startup", "nine_acceptances": "ten independently labelled",
                                "no_residual_refusal": "residual-ink refusal"}[case]
                    self.assertTrue(any(expected in error for error in diagnostic["verification"]["errors"]), diagnostic)

    def test_historical_four_file_inventory_is_explicit_and_current_inventory_is_complete(self):
        with tempfile.TemporaryDirectory() as temp:
            args, document, _ = fixture(Path(temp))
            document["frozen_reader_files"] = {name: args[3][name] for name in LEGACY_CORE_READER_FILES}
            write(args[0], document)
            historical = args[0].read_bytes()
            binding = reference_reread_binding(args[0], args[3])
            self.assertEqual(set(binding["historical_frozen_reader_files"]), set(LEGACY_CORE_READER_FILES))
            self.assertEqual(set(binding["current_reader_files"]), set(CORE_READER_FILES))
            validate_reference(*args, reader_reanalysis=binding)
            self.assertEqual(args[0].read_bytes(), historical)
            for missing in CORE_READER_FILES:
                method = dict(args[3])
                method.pop(missing)
                with self.subTest(missing=missing), self.assertRaises(ValueError):
                    reference_reread_binding(args[0], method)
            for inventory in (LEGACY_CORE_READER_FILES[:-1], (*LEGACY_CORE_READER_FILES, CORE_READER_FILES[-1])):
                document["frozen_reader_files"] = {name: args[3][name] for name in inventory}
                write(args[0], document)
                with self.subTest(inventory=inventory), self.assertRaises(ValueError):
                    reference_reread_binding(args[0], args[3])

    @staticmethod
    def startup_input(args, document):
        packet = args[0].parent
        still = packet / "startup/session.jpg"
        still.parent.mkdir()
        still.write_bytes(b"original startup pixels")
        registration = {"result": "PASS", "landmark_bounds": [382, 194, 603, 273]}
        preflight = write(packet / "startup/preflight.json", {
            "result": "PASS", "registration": registration,
            "source_still": {"name": still.name, "sha256": sha(still)}})
        startup = {"preflight": {"path": str(preflight.relative_to(packet)), "sha256": sha(preflight)},
                   "still": {"path": str(still.relative_to(packet)), "sha256": sha(still)}}
        for item in document["items"][1:3]:
            item["registration"] = deepcopy(registration)
            item["startup_calibration"] = deepcopy(startup)
        write(args[0], document)
        return registration, preflight, still

    def test_startup_sources_are_copied_and_geometry_is_recomputed_once_per_source(self):
        with tempfile.TemporaryDirectory() as temp:
            args, document, _ = fixture(Path(temp))
            registration, preflight, still = self.startup_input(args, document)
            destination = Path(temp) / "retained/reference.json"
            copy_reference(args[0], destination)
            self.assertEqual((destination.parent / "startup/session.jpg").read_bytes(), still.read_bytes())
            self.assertEqual((destination.parent / "startup/preflight.json").read_bytes(), preflight.read_bytes())
            calls, observations = [], []
            def calibrate(source, path):
                calls.append((source, path))
                return {**source["registration"], "primary_frequency_calibration": {"qualified": True}}
            original = args[-1]
            def observe(image, geometry):
                observations.append(deepcopy(geometry))
                return original(image, geometry)
            with patch.dict(sys.modules, {"encounter_frequency_idle": SimpleNamespace(registration_for_camera=calibrate)}):
                validate_reference(destination, *args[1:-1], observe)
            self.assertEqual(len(calls), 1)
            self.assertEqual(calls[0][0], json.loads(preflight.read_bytes()))
            self.assertEqual(calls[0][1], (destination.parent / "startup/session.jpg").resolve())
            self.assertEqual(sum("primary_frequency_calibration" in row for row in observations), 2)
            self.assertEqual(observations[0], args[4])

    def test_stored_or_mismatched_startup_geometry_cannot_enter_the_reader(self):
        for case in ("stored_matrix", "changed_still", "wrong_source_hash", "wrong_source_name",
                     "different_registration", "failed_preflight", "missing_ref", "changed_other_geometry"):
            with self.subTest(case=case), tempfile.TemporaryDirectory() as temp:
                args, document, _ = fixture(Path(temp))
                registration, preflight, still = self.startup_input(args, document)
                first = document["items"][1]
                source = json.loads(preflight.read_bytes())
                if case == "stored_matrix": first["registration"]["primary_frequency_calibration"] = {"qualified": True}
                elif case == "changed_still": still.write_bytes(b"different pixels")
                elif case == "wrong_source_hash": source["source_still"]["sha256"] = "b" * 64
                elif case == "wrong_source_name": source["source_still"]["name"] = "different.jpg"
                elif case == "different_registration": first["registration"]["landmark_bounds"][0] += 1
                elif case == "failed_preflight": source["result"] = "FAIL"
                elif case == "missing_ref": first["startup_calibration"].pop("still")
                write(preflight, source)
                for item in document["items"][1:3]:
                    item["startup_calibration"]["preflight"]["sha256"] = sha(preflight)
                write(args[0], document)
                def calibrate(source, path):
                    result = {**source["registration"], "primary_frequency_calibration": {"qualified": True}}
                    if case == "changed_other_geometry": result["landmark_bounds"] = [0, 0, 1, 1]
                    return result
                with patch.dict(sys.modules, {"encounter_frequency_idle": SimpleNamespace(registration_for_camera=calibrate)}):
                    with self.assertRaises(ValueError):
                        validate_reference(*args)

    @staticmethod
    def replace_label(args, document, role, label):
        item = next(item for item in document["items"] if item["role"] == role)
        path = args[0].parent / "observations.json"
        labels = json.loads(path.read_bytes())
        next(frame for frame in labels["frames"] if frame["frame_id"] == item["frame_id"])["primary_frequency"] = label
        write(path, labels)
        document["observations"]["sha256"] = sha(path)
        write(args[0], document)

    def test_exact_independent_labels_and_controls_do_not_change_old_labels(self):
        with tempfile.TemporaryDirectory() as temp:
            args, document, readings = fixture(Path(temp))
            original = args[2].read_bytes()
            result = validate_reference(*args)
            self.assertEqual(result["overrides"]["old-1"]["value"], "--.---")
            self.assertEqual(result["summary"]["held_out_dash_agreements"], 2)
            self.assertEqual(result["summary"]["items"], 9)
            self.assertEqual(args[2].read_bytes(), original)

    def test_optional_frozen_selection_is_copied_and_missing_or_changed_bytes_reject(self):
        for failure in ("missing", "changed"):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as temp:
                args, document, readings = fixture(Path(temp))
                selection = write(args[0].parent / "selection-before-reading.json", {"frozen_before_pixel_access": True})
                document["selection_before_reading"] = {"path": selection.name, "sha256": sha(selection)}
                write(args[0], document)
                copied = Path(temp) / "retained/reference.json"
                copy_reference(args[0], copied)
                retained_selection = copied.parent / selection.name
                self.assertEqual(retained_selection.read_bytes(), selection.read_bytes())
                validate_reference(copied, *args[1:])
                if failure == "missing":
                    retained_selection.unlink()
                else:
                    retained_selection.write_text("changed selection")
                with self.assertRaises(ValueError):
                    validate_reference(copied, *args[1:])
                with self.assertRaises(ValueError):
                    copy_reference(copied, Path(temp) / "invalid-copy/reference.json")

    def test_unresolved_adjudication_is_preserved_without_forcing_a_dash_label(self):
        with tempfile.TemporaryDirectory() as temp:
            args, document, readings = fixture(Path(temp))
            label = {"state": "ambiguous", "value": None, "reason": "One stroke is not resolved"}
            labels_path = args[0].parent / "observations.json"
            labels = json.loads(labels_path.read_bytes())
            labels["frames"][0]["primary_frequency"] = label
            write(labels_path, labels)
            document["observations"]["sha256"] = sha(labels_path)
            write(args[0], document)
            readings[document["items"][0]["image"]["sha256"]]["primary_frequency"] = deepcopy(label)
            self.assertEqual(validate_reference(*args)["overrides"]["old-1"], label)

    def test_each_control_rejects_an_asserted_dash(self):
        for role in CONTROL_STATES:
            with self.subTest(role=role), tempfile.TemporaryDirectory() as temp:
                args, document, readings = fixture(Path(temp))
                item = next(item for item in document["items"] if item["role"] == role)
                readings[item["image"]["sha256"]]["primary_frequency"] = {"state": "readable", "value": "--.---"}
                with self.assertRaises(ValueError):
                    validate_reference(*args)

    def test_frozen_wrong_literal_and_unresolved_labels_remain_truthful_reader_rejections(self):
        with tempfile.TemporaryDirectory() as temp:
            args, document, readings = fixture(Path(temp))
            for role, value in (("missing_glyph_control", "-.---"), ("missing_decimal_control", "-----")):
                self.replace_label(args, document, role, {"state": "readable", "value": value,
                                                         "reason": "Literal incomplete placeholder; do not supply missing characters"})
            for role in ("partial_glyph_control", "invalid_glyph_control", "partial_decimal_control"):
                self.replace_label(args, document, role, {"state": "unresolved", "value": None,
                                                         "reason": "Incomplete marks cannot resolve the literal"})
            frozen = (args[0].parent / "observations.json").read_bytes()
            result = validate_reference(*args)
            self.assertEqual(result["summary"]["counts"]["READER_REFUSAL"], 2)
            self.assertEqual(result["summary"]["counts"]["REFERENCE_UNRESOLVED"], 3)
            self.assertEqual((args[0].parent / "observations.json").read_bytes(), frozen)

    def test_negative_controls_never_accept_complete_placeholder_or_unrecognized_label(self):
        for role in ("missing_glyph_control", "missing_decimal_control", "partial_glyph_control"):
            for label in ({"state": "readable", "value": "--.---"},
                          {"state": "readable", "value": "gibberish"},
                          {"state": "maybe", "value": None},
                          {"state": "unresolved", "value": "--.---"}):
                with self.subTest(role=role, label=label), tempfile.TemporaryDirectory() as temp:
                    args, document, readings = fixture(Path(temp))
                    self.replace_label(args, document, role, {**label, "reason": "Unsupported control label"})
                    with self.assertRaises(ValueError):
                        validate_reference(*args)

    def test_binding_and_coverage_failures_reject(self):
        for case in ("source_labels", "source_manifest", "reader", "protocol", "labels", "image", "duplicate",
                     "correction_image", "control_missing", "control_operation", "heldout_exposed", "heldout_unknown", "blindness", "escape"):
            with self.subTest(case=case), tempfile.TemporaryDirectory() as temp:
                args, document, readings = fixture(Path(temp))
                if case == "source_labels": document["original_blind_observations_sha256"] = "d" * 64
                elif case == "source_manifest": document["original_blind_manifest_sha256"] = "d" * 64
                elif case == "reader": args[3][CORE_READER_FILES[0]] = "d" * 64
                elif case in ("protocol", "labels"):
                    document["observations" if case == "labels" else case]["sha256"] = "d" * 64
                elif case == "image": (args[0].parent / document["items"][0]["image"]["path"]).write_bytes(b"changed")
                elif case == "duplicate": document["items"].append(document["items"][0])
                elif case == "correction_image": document["items"][0]["source_frame_id"] = "other"
                elif case == "control_missing": document["items"][-1]["role"] = "held_out_original"
                elif case == "control_operation": document["items"][-1].pop("operation")
                elif case == "heldout_exposed": document["items"][1]["not_used_for_reader_development"] = False
                elif case == "heldout_unknown": readings[document["items"][1]["image"]["sha256"]]["primary_frequency"] = {"state": "ambiguous", "value": None}
                elif case == "escape": document["protocol"]["path"] = "../outside.md"
                else:
                    labels_path = args[0].parent / "observations.json"
                    labels = json.loads(labels_path.read_bytes())
                    labels["blind_protocol"]["observer_received_source_or_expectations"] = True
                    write(labels_path, labels)
                    document["observations"]["sha256"] = sha(labels_path)
                write(args[0], document)
                with self.assertRaises(ValueError):
                    validate_reference(*args)

    def test_current_reread_preserves_historical_packet_and_does_not_claim_new_heldout_data(self):
        with tempfile.TemporaryDirectory() as temp:
            args, document, readings = fixture(Path(temp))
            frozen_packet = args[0].read_bytes()
            frozen_labels = (args[0].parent / "observations.json").read_bytes()
            args[3][CORE_READER_FILES[0]] = "d" * 64
            with self.assertRaisesRegex(ValueError, "reader changed"):
                validate_reference(*args)
            binding = reference_reread_binding(args[0], args[3])
            seen = []
            original_observe = args[-1]
            args[-1] = lambda image, registration: (seen.append(sha(image)), original_observe(image, registration))[1]
            result = validate_reference(*args, reader_reanalysis=binding)
            self.assertEqual(len(seen), len(document["items"]))
            self.assertEqual(set(seen), {item["image"]["sha256"] for item in document["items"]})
            self.assertEqual(result["summary"]["reader_reanalysis"], binding)
            self.assertIn("Original frozen reader only", binding["held_out_provenance"])
            self.assertEqual(binding["historical_frozen_reader_files"], document["frozen_reader_files"])
            self.assertEqual(args[0].read_bytes(), frozen_packet)
            self.assertEqual((args[0].parent / "observations.json").read_bytes(), frozen_labels)

    def test_reread_rejects_stale_binding_changed_input_and_new_reader_false_assertions(self):
        for failure in ("binding_hash", "binding_reader", "partial_reread", "changed_reference", "changed_image", "false_assertion"):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as temp:
                args, document, readings = fixture(Path(temp))
                args[3][CORE_READER_FILES[0]] = "d" * 64
                binding = reference_reread_binding(args[0], args[3])
                if failure == "binding_hash": binding["reference_sha256"] = "e" * 64
                elif failure == "binding_reader": binding["current_reader_files"][CORE_READER_FILES[0]] = "e" * 64
                elif failure == "partial_reread": binding["complete_source_set_reread"] = False
                elif failure == "changed_reference":
                    document["frozen_reader_files"][CORE_READER_FILES[1]] = "e" * 64
                    write(args[0], document)
                elif failure == "changed_image":
                    (args[0].parent / document["items"][0]["image"]["path"]).write_bytes(b"changed original")
                else:
                    item = next(item for item in document["items"] if item["role"] == "partial_decimal_control")
                    readings[item["image"]["sha256"]]["primary_frequency"] = {"state": "readable", "value": "--.---"}
                with self.assertRaises(ValueError):
                    validate_reference(*args, reader_reanalysis=binding)

    def test_qualification_rejects_reread_binding_without_independent_packet(self):
        from test_encounter_qualification import QualificationTests
        helper = QualificationTests("test_complete_exact_bundle_qualifies")
        helper.setUp()
        self.addCleanup(helper.tearDown)
        field = helper.field_validation()
        field["primary_frequency_reader_reanalysis"] = {"kind": "complete_exact_reader_reread"}
        result = helper.verify(helper.write_bundle(field))
        self.assertEqual(result["status"], "REJECTED")
        self.assertTrue(any("lacks its independent reference" in error for error in result["errors"]), result)

    def test_real_workflow_and_verifier_preserve_original_and_explicit_adjudication(self):
        import encounter_qualification_workflow as workflow
        from test_encounter_qualification_workflow import QualificationWorkflowTests

        support = QualificationWorkflowTests("test_static_reanalysis_publishes_only_verified_current_bundle")
        self.addCleanup(support.doCleanups)
        with tempfile.TemporaryDirectory() as temp:
            destination = Path(temp) / "qualified"
            helper, source, method, runtime, patches, session = support._static_reanalysis_fixture(destination)
            field_path = workflow.resolve_reference(source.parent, workflow.read_json(source)["field_validation"], "field")
            field = workflow.read_json(field_path)
            original_manifest = workflow.resolve_reference(field_path.parent, field["source_artifacts"]["blind_manifest"], "manifest")
            original_labels = workflow.resolve_reference(field_path.parent, field["source_artifacts"]["blind_observations"], "labels")
            original_image = workflow.resolve_reference(field_path.parent, field["frames"][0]["image"], "image")
            old_bytes = original_labels.read_bytes()
            args, document, _ = fixture(Path(temp) / "supplement", original_manifest, original_labels,
                                        original_image, field["frames"][0]["frame_id"], method, helper.reader_observations)
            with ExitStack() as stack:
                for item in patches:
                    stack.enter_context(item)
                result = workflow.reanalyze_static(source, destination, args[0])
            self.assertEqual(result["status"], "QUALIFIED")
            manifest = workflow.read_json(Path(result["manifest"]))
            new_field_path = workflow.resolve_reference(destination, manifest["field_validation"], "field")
            new_field = workflow.read_json(new_field_path)
            preserved = workflow.resolve_reference(new_field_path.parent, new_field["source_artifacts"]["blind_observations"], "labels")
            self.assertEqual(preserved.read_bytes(), old_bytes)
            self.assertEqual(original_labels.read_bytes(), old_bytes)
            frequency = next(c for c in new_field["frames"][0]["checks"] if c["field"] == "primary_frequency")
            self.assertEqual(frequency["reference"]["value"], "--.---")
            self.assertEqual(frequency["original_reference"]["value"], "34.700")
            self.assertEqual(new_field["primary_frequency_adjudication"]["held_out_dash_agreements"], 2)
            # A later reader rereads the same independent packet. Its original
            # held-out declaration still belongs to the historical reader.
            second = Path(temp) / "qualified-again"
            _, chained_source, new_method, _, chained_patches, _ = support._static_reanalysis_fixture(
                second, helper=helper, source_manifest=Path(result["manifest"]), method_version=7, reader_sha="d" * 64)
            with ExitStack() as stack:
                for item in chained_patches:
                    stack.enter_context(item)
                chained = workflow.reanalyze_static(chained_source, second)
                self.assertEqual(chained["status"], "QUALIFIED")
            chained_manifest = workflow.read_json(Path(chained["manifest"]))
            chained_field_path = workflow.resolve_reference(second, chained_manifest["field_validation"], "field")
            chained_field = workflow.read_json(chained_field_path)
            retained_packet = workflow.resolve_reference(chained_field_path.parent,
                chained_field["source_artifacts"]["primary_frequency_reference"], "packet")
            self.assertEqual(retained_packet.read_bytes(), args[0].read_bytes())
            self.assertEqual(chained_field["primary_frequency_reader_reanalysis"], reference_reread_binding(retained_packet, new_method))
            self.assertEqual(chained_field["primary_frequency_adjudication"]["reader_reanalysis"],
                             chained_field["primary_frequency_reader_reanalysis"])
            parsed = workflow.build_parser().parse_args([
                "reanalyze-static", "--out", str(second), "--primary-frequency-reference", str(args[0])])
            self.assertEqual(parsed.primary_frequency_reference, args[0])


class RetainedFrequencySupplementTests(unittest.TestCase):
    """Retain independently labelled multi-capture evidence without false admissions."""

    @staticmethod
    def append(args, document, readings, role, label, observed=None):
        packet = args[0].parent
        frame_id = 'new-' + str(len(document['items']))
        image = packet / (frame_id + '.png')
        image.write_bytes(('different opaque pixels ' + frame_id).encode())
        image_ref = {'path': image.name, 'sha256': sha(image)}
        item = {'frame_id': frame_id, 'role': role, 'image': image_ref,
                'operation': {'declared_pixel_change': role},
                'registration': {'result': 'PASS', 'landmark_bounds': [382, 193, 601, 272]}}
        if role == 'retained_original':
            item.update(origin={'capture_id': 'other-capture', 'video_frame_index': 12484,
                                'video_sha256': 'b' * 64},
                        development_provenance='Independently labelled before a prior reader; current complete reread.')
        document['items'].append(item)
        manifest_path = packet / document['blind_manifest']['path']
        manifest = json.loads(manifest_path.read_bytes())
        manifest['frames'].append({'frame_id': frame_id, 'image': image_ref})
        write(manifest_path, manifest)
        document['blind_manifest']['sha256'] = sha(manifest_path)
        labels_path = packet / document['observations']['path']
        labels = json.loads(labels_path.read_bytes())
        labels['frames'].append({'frame_id': frame_id, 'image_sha256': image_ref['sha256'],
                                 'primary_frequency': deepcopy(label)})
        write(labels_path, labels)
        document['observations']['sha256'] = sha(labels_path)
        readings[image_ref['sha256']] = {'primary_frequency': deepcopy(label if observed is None else observed)}
        write(args[0], document)
        return item

    def test_retained_original_uses_own_registration_and_preserves_safe_refusal(self):
        with tempfile.TemporaryDirectory() as temp:
            args, document, readings = fixture(Path(temp))
            label = {'state': 'readable', 'value': '24.150', 'reason': 'Independent full literal'}
            refusal = {'state': 'ambiguous', 'value': None, 'reason': 'One stroke not resolved'}
            item = self.append(args, document, readings, 'retained_original', label, refusal)
            seen = []
            def observe(image, registration):
                if sha(image) == item['image']['sha256']:
                    seen.append(registration)
                return deepcopy(readings[sha(image)])
            args[-1] = observe
            result = validate_reference(*args)
            self.assertEqual(seen, [item['registration']])
            self.assertEqual(result['summary']['counts']['READER_REFUSAL'], 1)
            self.assertEqual(result['summary']['held_out_dash_agreements'], 2)
            readings[item['image']['sha256']]['primary_frequency'] = {**label, 'value': '24.158'}
            with self.assertRaises(ValueError):
                validate_reference(*args)

    def test_new_ambiguous_original_cannot_support_any_assertion(self):
        with tempfile.TemporaryDirectory() as temp:
            args, document, readings = fixture(Path(temp))
            label = {'state': 'ambiguous', 'value': None, 'reason': 'Independent observer could not establish complete marks'}
            item = self.append(args, document, readings, 'retained_original', label)
            validate_reference(*args)
            for state, value in [('readable', '--.---'), ('readable', '24.150'), ('absent', None)]:
                with self.subTest(state=state, value=value):
                    readings[item['image']['sha256']]['primary_frequency'] = {'state': state, 'value': value}
                    with self.assertRaises(ValueError):
                        validate_reference(*args)

    def test_dim_controls_preserve_complete_wrong_literal_and_refuse_extra_or_occluded_ink(self):
        with tempfile.TemporaryDirectory() as temp:
            args, document, readings = fixture(Path(temp))
            numeric = {'state': 'readable', 'value': '24.158', 'reason': 'Visible complete extra middle stroke'}
            refusal = {'state': 'ambiguous', 'value': None, 'reason': 'Incomplete or hidden display'}
            wrong = self.append(args, document, readings, 'wrong_literal_control', numeric)
            extra = self.append(args, document, readings, 'extra_ink_control',
                                {**numeric, 'value': '24.150', 'reason': 'Literal with a separate square inside a hole'}, refusal)
            hidden = self.append(args, document, readings, 'occluded_camera_control', refusal)
            validate_reference(*args)
            for item, output in [(wrong, refusal), (wrong, {**numeric, 'value': '24.150'}),
                                 (extra, {**numeric, 'value': '24.150'}),
                                 (hidden, {'state': 'absent', 'value': None})]:
                with self.subTest(role=item['role'], output=output):
                    key = item['image']['sha256']
                    previous = readings[key]['primary_frequency']
                    readings[key]['primary_frequency'] = output
                    with self.assertRaises(ValueError):
                        validate_reference(*args)
                    readings[key]['primary_frequency'] = previous

    def test_historical_packets_are_copied_byte_for_byte_and_tampering_rejects(self):
        with tempfile.TemporaryDirectory() as temp:
            args, document, readings = fixture(Path(temp))
            historical = write(args[0].parent / 'history/first-observer.json', {'state': 'ambiguous', 'never_relabelled': True})
            document['provenance_artifacts'] = [{'path': 'history/first-observer.json', 'sha256': sha(historical)}]
            write(args[0], document)
            copied = Path(temp) / 'copied/reference.json'
            copy_reference(args[0], copied)
            self.assertEqual((copied.parent / 'history/first-observer.json').read_bytes(), historical.read_bytes())
            validate_reference(copied, *args[1:])
            with patch.object(frequency_reference, "_clone_file", side_effect=AssertionError("rewritten")), \
                    patch.object(frequency_reference.shutil, "copy2", side_effect=AssertionError("rewritten")):
                copy_reference(args[0], copied)
            (copied.parent / 'history/first-observer.json').write_text('silently changed observer')
            with self.assertRaises(ValueError):
                validate_reference(copied, *args[1:])
            with self.assertRaises(ValueError):
                copy_reference(copied, Path(temp) / 'invalid/reference.json')

if __name__ == "__main__":
    unittest.main()
