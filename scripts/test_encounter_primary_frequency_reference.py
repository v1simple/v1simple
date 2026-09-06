#!/usr/bin/env python3
"""Independent reference corrections preserve history and reject false admissions."""
from contextlib import ExitStack
from copy import deepcopy
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).parent))
sys.path.insert(0, str(Path(__file__).parent / "bench"))
from encounter_primary_frequency_reference import BLIND_PROTOCOL, CONTROL_STATES, validate_reference
from encounter_qualification import CORE_READER_FILES


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


class PrimaryFrequencyReferenceTests(unittest.TestCase):
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
            # A later host-only reanalysis carries the same independent packet,
            # not an untraceable copy of its corrected labels.
            second = Path(temp) / "qualified-again"
            _, chained_source, _, _, chained_patches, _ = support._static_reanalysis_fixture(
                second, helper=helper, source_manifest=Path(result["manifest"]))
            with ExitStack() as stack:
                for item in chained_patches:
                    stack.enter_context(item)
                self.assertEqual(workflow.reanalyze_static(chained_source, second)["status"], "QUALIFIED")
            parsed = workflow.build_parser().parse_args([
                "reanalyze-static", "--out", str(second), "--primary-frequency-reference", str(args[0])])
            self.assertEqual(parsed.primary_frequency_reference, args[0])


if __name__ == "__main__":
    unittest.main()
