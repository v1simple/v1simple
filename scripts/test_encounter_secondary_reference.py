#!/usr/bin/env python3
"""Supplementary card evidence preserves literal labels and original pixels."""
from contextlib import ExitStack
from collections import Counter
from copy import deepcopy
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

from PIL import Image

sys.path.insert(0, str(Path(__file__).parent / "bench"))
from encounter_qualification import CORE_READER_FILES
from encounter_secondary_reference import (
    BLIND_PROTOCOL, copy_reference, reference_reread_binding, validate_reference)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value))
    return path


def fixture(root, method=None, camera=None, roles=("development_original", "held_out_original")):
    root.mkdir(parents=True, exist_ok=True)
    method = method or {name: "a" * 64 for name in CORE_READER_FILES}
    camera = camera or {"name": "test camera", "profile": {"framerate": 200}}
    def ref(path):
        return {"path": str(path.relative_to(root)), "sha256": sha(path)}
    reg = {"result": "PASS", "landmark_bounds": [384, 194, 603, 273]}
    startup = root / "startup/source.png"; startup.parent.mkdir()
    Image.new("RGB", (1280, 720)).save(startup)
    preflight = write(root / "startup/preflight.json", {
        "result": "PASS", "registration": reg, "camera": camera,
        "source_still": {"name": startup.name, "sha256": sha(startup), "size_bytes": startup.stat().st_size}}
    )
    items, manifest_items, labels, readings = [], [], [], {}
    for index, role in enumerate(roles):
        opaque_id = f"R{index + 1:03d}"
        original = root / f"originals/{opaque_id}.png"; original.parent.mkdir(exist_ok=True)
        Image.new("RGB", (24, 16), (index, 10, 20)).save(original)
        crop = root / f"observer/{opaque_id}.png"; crop.parent.mkdir(exist_ok=True)
        with Image.open(original) as image:
            image.crop((2, 3, 10, 12)).save(crop)
        value = {"band": "K", "frequency": "24.150", "direction": "side", "bars": 3}
        slots = [{"slot": 0, "presence": "present", **value},
                 {"slot": 1, "presence": "absent", **dict.fromkeys(value)}]
        label = {"id": opaque_id, "field": "secondary", "image_sha256": sha(crop),
                 "state": "readable", "value": [value], "reason": "Literal observed card", "cards": slots}
        card = {"slot": 0, **value,
                "split_text_observation": {"method": "literal_split_card_text/v1", "accepted": True}}
        observed = {"state": "readable", "value": [deepcopy(value)], "cards": [card]}
        if index:
            label["state"], label["value"] = "unreadable", None
            label["cards"][0]["bars"] = None
            observed["state"], observed["value"] = "unreadable", None
            observed["partial_cards"] = [{**value, "bars": None}]
            observed["cards"][0]["bars"] = None
        readings[sha(original)] = {"secondary": observed}
        labels.append(label)
        manifest_items.append({"id": opaque_id, "field": "secondary", "image": crop.name, "sha256": sha(crop)})
        items.append({"id": opaque_id, "image": ref(original), "observer_crop_box": [2, 3, 10, 12],
                      "registration": deepcopy(reg), "startup_calibration": {"preflight": ref(preflight), "still": ref(startup)},
                      "origin": {"role": role, "capture_id": f"source-{index}",
                                 "video_sha256": f"{index:064x}", "video_frame_index": index}})
    manifest = write(root / "observer/manifest.json", {"schema_version": 1, "items": manifest_items})
    observations = write(root / "observer/observations.json", {
        "schema_version": 1, "blind_protocol": BLIND_PROTOCOL, "items": labels})
    protocol = root / "observer/README.txt"; protocol.write_text("Observe each original crop independently.")
    selection = write(root / "selection.json", {"items": [item["id"] for item in items]})
    document = {"schema_version": 1, "kind": "independent_secondary_reference",
                "protocol": ref(protocol), "blind_manifest": ref(manifest), "observations": ref(observations),
                "selection_before_reading": ref(selection),
                "frozen_reader_files": {name: method[name] for name in CORE_READER_FILES}, "items": items}
    path = write(root / "reference.json", document)
    def observe(image, registration):
        # Reader claims are inputs to the verifier. Image/startup/crop checks use
        # real files and the actual shared registration loader, not stubs.
        assert registration["landmark_bounds"] == reg["landmark_bounds"]
        return deepcopy(readings[sha(image)])
    return path, document, method, readings, observe, camera


class SecondaryReferenceTests(unittest.TestCase):
    def legacy_packet(self):
        import test_encounter_qualification as support
        helper = support.QualificationTests("test_complete_exact_bundle_qualifies")
        helper.setUp(); self.addCleanup(helper.tearDown)
        document = helper.visible_secondary_validation()
        sources = {name: json.loads((helper.root / ref["path"]).read_text())
                   for name, ref in document["source_artifacts"].items()}
        for index in range(12, 42):
            item = deepcopy(document["items"][index % 12])
            opaque = f"retained-secondary-{index:02d}.png"
            image = helper.write_image(f"secondary/images/{opaque}", opaque)
            item.update(item_id=opaque, source_image=opaque, image=helper.reference(image, helper.root),
                        image_sha256=sha(image))
            item["blind_label"]["image"] = opaque
            document["items"].append(item)
            helper.reader_observations[sha(image)] = {"secondary": deepcopy(item["observed"])}
            sources["packet_manifest"]["image_integrity"][opaque] = {"sha256": sha(image)}
            sources["blind_labels"]["single_frame_items"].append({"item_id": opaque, "frame": item["blind_label"]})
            sources["sealed_key"]["single_frame_controls"].append({
                "item_id": opaque, "image": opaque, "machine_secondary": item["observed"]})
        for name, content in sources.items():
            source_path = helper.root / document["source_artifacts"][name]["path"]
            write(source_path, content)
            document["source_artifacts"][name]["sha256"] = sha(source_path)
        document["source"] = {key: document["source_artifacts"][name]["sha256"] for key, name in (
            ("packet_manifest_sha256", "packet_manifest"), ("blind_labels_sha256", "blind_labels"),
            ("sealed_key_sha256", "sealed_key"))}
        document["counts"] = dict(Counter(item["status"] for item in document["items"]))
        document["unique_original_frames"] = 42
        return helper, document, support

    def test_workflow_adds_supplement_without_rewriting_historical_42_or_four_sources(self):
        import encounter_qualification as qualification
        import encounter_qualification_workflow as workflow
        helper, historical, support = self.legacy_packet()
        source = write(helper.root / "visible-secondary.json", historical)
        packet, _, _, readings, _, _ = fixture(helper.root / "supplement", helper.method, support.CAMERA)
        helper.reader_observations.update(readings)
        before = {p.relative_to(helper.root): p.read_bytes() for p in helper.root.rglob("*") if p.is_file()}
        def observe(path, registration):
            return deepcopy(helper.reader_observations[sha(path)])
        with tempfile.TemporaryDirectory() as temp, patch.object(qualification, "_observe_image", side_effect=observe):
            destination = Path(temp) / "secondary/visible-secondary.json"
            result = workflow._reanalyze_secondary_document(
                source, destination, support.READER, helper.method, support.CAMERA, packet)
            self.assertEqual(result["items"], historical["items"])
            self.assertEqual(result["source_artifacts"], historical["source_artifacts"])
            self.assertEqual(set(result["source_artifacts"]), set(qualification.VISIBLE_SECONDARY_SOURCE_NAMES))
            verified = qualification._validate_visible_secondary_evidence(
                result, support.READER, support.CAMERA, helper.method, destination.parent)
            self.assertEqual(verified["unique_original_frames"], 42)
            self.assertEqual(verified["secondary_reference"]["unique_original_frames"], 2)
            # A subsequent reread copies the same independent supplement too.
            second = workflow._reanalyze_secondary_document(
                destination, Path(temp) / "second/visible-secondary.json",
                support.READER, helper.method, support.CAMERA)
            self.assertEqual(second["secondary_reference_summary"], result["secondary_reference_summary"])
            result["secondary_reference_summary"]["split_text_agreements"] += 100
            with self.assertRaisesRegex(qualification.QualificationError, "summary differs"):
                qualification._validate_visible_secondary_evidence(
                    result, support.READER, support.CAMERA, helper.method, destination.parent)
        self.assertEqual(before, {p.relative_to(helper.root): p.read_bytes()
                                  for p in helper.root.rglob("*") if p.is_file()})

    def test_reader26_requires_actual_supported_complete_band_use_and_retains_split_gate(self):
        import encounter_qualification as qualification
        import encounter_qualification_workflow as workflow
        for case in ("missing", "refused", "wrong_method", "accepted", "missing_split"):
            with self.subTest(case=case):
                helper, historical, support = self.legacy_packet()
                source = write(helper.root / "visible-secondary.json", historical)
                packet, _, _, readings, _, _ = fixture(helper.root / "supplement", helper.method, support.CAMERA)
                for observation in readings.values():
                    card = observation["secondary"]["cards"][0]
                    if case != "missing":
                        card["band_pixel_observation"] = {
                            "method": "unqualified_method" if case == "wrong_method" else "complete_card_band/v1",
                            "accepted": case != "refused"}
                    if case == "missing_split":
                        card["split_text_observation"]["accepted"] = False
                helper.reader_observations.update(readings)
                runtime = {**support.READER, "method_version": 26}
                def observe(path, registration):
                    return deepcopy(helper.reader_observations[sha(path)])
                with tempfile.TemporaryDirectory() as temp, patch.object(qualification, "_observe_image", side_effect=observe):
                    destination = Path(temp) / "visible-secondary.json"
                    document = workflow._reanalyze_secondary_document(
                        source, destination, runtime, helper.method, support.CAMERA, packet)
                    if case == "accepted":
                        result = qualification._validate_visible_secondary_evidence(
                            document, runtime, support.CAMERA, helper.method, destination.parent)
                        summary = result["secondary_reference"]
                        self.assertEqual(summary["band_pixel_agreements"], 2)
                        self.assertEqual(summary["band_pixel_agreements_by_role"], {
                            "development_original": 1, "held_out_original": 1})
                        self.assertEqual(summary["split_text_agreements"], 2)
                    else:
                        branch = "split-text" if case == "missing_split" else "complete-band"
                        with self.assertRaisesRegex(qualification.QualificationError, "does not exercise the " + branch):
                            qualification._validate_visible_secondary_evidence(
                                document, runtime, support.CAMERA, helper.method, destination.parent)
                    document["secondary_reference_summary"]["band_pixel_agreements"] += 100
                    with self.assertRaisesRegex(qualification.QualificationError, "summary differs"):
                        qualification._validate_visible_secondary_evidence(
                            document, runtime, support.CAMERA, helper.method, destination.parent)

    def test_reader26_rereads_historical_42_plus_100_when_80_originals_are_appended(self):
        import encounter_qualification as qualification
        import encounter_qualification_workflow as workflow
        helper, historical, support = self.legacy_packet()
        roles = ("development_original",) * 60 + ("held_out_original",) * 40
        old_path, old, _, old_readings, old_observe, _ = fixture(
            helper.root / "old", helper.method, support.CAMERA, roles)
        new_path, new, _, new_readings, _, _ = fixture(
            helper.root / "new", helper.method, support.CAMERA, roles + ("held_out_original",) * 80)
        old_labels = json.loads((old_path.parent / old["observations"]["path"]).read_text())
        new_labels = json.loads((new_path.parent / new["observations"]["path"]).read_text())
        self.assertEqual(new["items"][:100], old["items"])
        self.assertEqual(new_labels["items"][:100], old_labels["items"])
        historical["secondary_reference"] = helper.reference(old_path, helper.root)
        historical["secondary_reference_summary"] = validate_reference(
            old_path, helper.method, old_observe, camera=support.CAMERA)
        source = write(helper.root / "visible-secondary.json", historical)
        for item in new["items"][100:]:
            new_readings[item["image"]["sha256"]]["secondary"]["cards"][0]["band_pixel_observation"] = {
                "method": "complete_card_band/v1", "accepted": True}
        helper.reader_observations.update(old_readings)
        helper.reader_observations.update(new_readings)
        before = {p.relative_to(helper.root): p.read_bytes() for p in helper.root.rglob("*") if p.is_file()}
        runtime = {**support.READER, "method_version": 26}
        def observe(path, registration):
            return deepcopy(helper.reader_observations[sha(path)])
        with tempfile.TemporaryDirectory() as temp, patch.object(qualification, "_observe_image", side_effect=observe):
            destination = Path(temp) / "visible-secondary.json"
            document = workflow._reanalyze_secondary_document(
                source, destination, runtime, helper.method, support.CAMERA, new_path)
            verified = qualification._validate_visible_secondary_evidence(
                document, runtime, support.CAMERA, helper.method, destination.parent)
            self.assertEqual(document["items"], historical["items"])
            self.assertEqual(document["source_artifacts"], historical["source_artifacts"])
            self.assertEqual(verified["unique_original_frames"], 42)
            self.assertEqual(verified["secondary_reference"]["unique_original_frames"], 180)
            self.assertEqual(verified["secondary_reference"]["band_pixel_agreements"], 80)
            retained_new = destination.parent / document["secondary_reference"]["path"]
            self.assertEqual(retained_new.read_bytes(), new_path.read_bytes())
            retained_old = destination.parent / historical["secondary_reference"]["path"]
            self.assertEqual(retained_old.read_bytes(), old_path.read_bytes())
            self.assertEqual((retained_old.parent / old["observations"]["path"]).read_bytes(),
                             (old_path.parent / old["observations"]["path"]).read_bytes())
        self.assertEqual(before, {p.relative_to(helper.root): p.read_bytes()
                                  for p in helper.root.rglob("*") if p.is_file()})

    def test_complete_band_rejects_wrong_or_unsupported_full_and_partial_assertions(self):
        for index in (0, 1):
            for mutation in ("wrong_literal", "unresolved_reference", "wrong_branch_identity"):
                with self.subTest(index=index, mutation=mutation), tempfile.TemporaryDirectory() as temp:
                    path, doc, method, readings, observe, _ = fixture(Path(temp))
                    observed = readings[doc["items"][index]["image"]["sha256"]]["secondary"]
                    card = observed["cards"][0]
                    card["split_text_observation"]["accepted"] = False
                    card["band_pixel_observation"] = {"method": "complete_card_band/v1", "accepted": True}
                    if mutation == "wrong_literal":
                        value = observed["value"] if index == 0 else observed["partial_cards"]
                        value[0]["band"] = "Ka"
                    elif mutation == "wrong_branch_identity":
                        card["band"] = "Ka"
                    else:
                        labels_path = path.parent / doc["observations"]["path"]
                        labels = json.loads(labels_path.read_text())
                        label = labels["items"][index]
                        label["state"], label["value"] = "unreadable", None
                        label["cards"][0]["band"] = None
                        write(labels_path, labels)
                        doc["observations"]["sha256"] = sha(labels_path)
                        write(path, doc)
                    with self.assertRaisesRegex(ValueError, "contradicts|unsupported partial|unsupported complete-band"):
                        validate_reference(path, method, observe)

    def test_reader25_requires_actual_supported_split_branch_use(self):
        import encounter_qualification as qualification
        import encounter_qualification_workflow as workflow
        for include, actual in ((False, False), (True, False), (True, True)):
            with self.subTest(include=include, actual=actual):
                helper, historical, support = self.legacy_packet()
                source = write(helper.root / "visible-secondary.json", historical)
                packet, _, _, readings, _, _ = fixture(helper.root / "supplement", helper.method, support.CAMERA)
                if not actual:
                    for observation in readings.values():
                        observation["secondary"]["cards"][0]["split_text_observation"]["accepted"] = False
                helper.reader_observations.update(readings)
                runtime = {**support.READER, "method_version": 25}
                def observe(path, registration):
                    return deepcopy(helper.reader_observations[sha(path)])
                with tempfile.TemporaryDirectory() as temp, patch.object(qualification, "_observe_image", side_effect=observe):
                    destination = Path(temp) / "visible-secondary.json"
                    document = workflow._reanalyze_secondary_document(
                        source, destination, runtime, helper.method, support.CAMERA, packet if include else None)
                    if actual:
                        result = qualification._validate_visible_secondary_evidence(
                            document, runtime, support.CAMERA, helper.method, destination.parent)
                        self.assertEqual(result["secondary_reference"]["split_text_agreements"], 2)
                    else:
                        with self.assertRaisesRegex(qualification.QualificationError, "does not exercise the split-text"):
                            qualification._validate_visible_secondary_evidence(
                                document, runtime, support.CAMERA, helper.method, destination.parent)

    def test_full_and_partial_literal_assertions_are_checked(self):
        with tempfile.TemporaryDirectory() as temp:
            path, _, method, _, observe, camera = fixture(Path(temp))
            result = validate_reference(path, method, observe, camera=camera)
            self.assertEqual(result["counts"], {"AGREEMENT": 1, "REFERENCE_UNRESOLVED": 1})
            self.assertEqual(result["split_text_agreements"], 2)
            self.assertEqual(result["partial_identity_assertions"], 1)
            self.assertEqual(result["split_text_agreements_by_role"]["held_out_original"], 1)

    def test_left_and_right_slot_words_preserve_untouched_observer_labels(self):
        with tempfile.TemporaryDirectory() as temp:
            path, doc, method, _, observe, _ = fixture(Path(temp))
            labels_path = path.parent / doc["observations"]["path"]
            labels = json.loads(labels_path.read_text())
            for label in labels["items"]:
                for card in label["cards"]:
                    card["slot"] = ("left", "right")[card["slot"]]
            write(labels_path, labels); doc["observations"]["sha256"] = sha(labels_path); write(path, doc)
            before = labels_path.read_bytes()
            self.assertEqual(validate_reference(path, method, observe)["split_text_agreements"], 2)
            self.assertEqual(labels_path.read_bytes(), before)

    def test_wrong_full_or_partial_literal_is_rejected(self):
        for index in (0, 1):
            with self.subTest(index=index), tempfile.TemporaryDirectory() as temp:
                path, doc, method, readings, observe, _ = fixture(Path(temp))
                observed = readings[doc["items"][index]["image"]["sha256"]]["secondary"]
                target = observed["value"] if index == 0 else observed["partial_cards"]
                target[0]["frequency"] = "24.151"
                with self.assertRaisesRegex(ValueError, "contradicts|unsupported partial"):
                    validate_reference(path, method, observe)

    def test_unresolved_identity_cannot_support_partial_assertion(self):
        with tempfile.TemporaryDirectory() as temp:
            path, doc, method, _, observe, _ = fixture(Path(temp))
            labels_path = path.parent / doc["observations"]["path"]
            labels = json.loads(labels_path.read_text()); labels["items"][1]["cards"][0]["frequency"] = None
            write(labels_path, labels); doc["observations"]["sha256"] = sha(labels_path); write(path, doc)
            with self.assertRaisesRegex(ValueError, "unsupported partial"):
                validate_reference(path, method, observe)

    def test_changed_original_crop_or_startup_hash_refuses(self):
        for target in ("original", "crop", "startup", "preflight"):
            with self.subTest(target=target), tempfile.TemporaryDirectory() as temp:
                path, doc, method, _, observe, _ = fixture(Path(temp))
                item = doc["items"][0]
                ref = (item["image"] if target == "original" else item["startup_calibration"]["still"]
                       if target == "startup" else item["startup_calibration"]["preflight"] if target == "preflight" else
                       {"path": "observer/R001.png"})
                changed = path.parent / ref["path"]; changed.write_bytes(changed.read_bytes() + b"changed")
                with self.assertRaisesRegex(ValueError, "changed artifact"):
                    validate_reference(path, method, observe)

    def test_false_crop_mapping_refuses_even_with_all_files_hash_bound(self):
        with tempfile.TemporaryDirectory() as temp:
            path, doc, method, _, observe, _ = fixture(Path(temp))
            doc["items"][0]["observer_crop_box"] = [2, 3, 11, 12]; write(path, doc)
            with self.assertRaisesRegex(ValueError, "differs from original pixels"):
                validate_reference(path, method, observe)

    def test_substituted_registration_or_still_refuses(self):
        for mutation in ("registration", "still"):
            with self.subTest(mutation=mutation), tempfile.TemporaryDirectory() as temp:
                path, doc, method, _, observe, _ = fixture(Path(temp))
                if mutation == "registration":
                    doc["items"][0]["registration"]["landmark_bounds"][0] += 1
                else:
                    new = path.parent / "startup/other.png"; Image.new("RGB", (1280,720), "white").save(new)
                    doc["items"][0]["startup_calibration"]["still"] = {"path":"startup/other.png", "sha256":sha(new)}
                write(path, doc)
                with self.assertRaisesRegex(ValueError, "preflight differs|different original still"):
                    validate_reference(path, method, observe)

    def test_omitting_selected_image_or_forging_label_hash_refuses(self):
        for mutation in ("omit", "label"):
            with self.subTest(mutation=mutation), tempfile.TemporaryDirectory() as temp:
                path, doc, method, _, observe, _ = fixture(Path(temp))
                if mutation == "omit": doc["items"].pop()
                else:
                    labels_path = path.parent / doc["observations"]["path"]
                    labels = json.loads(labels_path.read_text()); labels["items"][0]["image_sha256"] = "0" * 64
                    write(labels_path, labels); doc["observations"]["sha256"] = sha(labels_path)
                write(path, doc)
                with self.assertRaisesRegex(ValueError, "selected secondary subset|label differs"):
                    validate_reference(path, method, observe)

    def test_changed_reader_requires_explicit_complete_reread(self):
        with tempfile.TemporaryDirectory() as temp:
            path, _, method, _, observe, _ = fixture(Path(temp))
            method["encounter_card_text.py"] = "b" * 64
            with self.assertRaisesRegex(ValueError, "differs from immutable"):
                validate_reference(path, method, observe)
            binding = reference_reread_binding(path, method)
            self.assertTrue(validate_reference(path, method, observe, reader_reanalysis=binding)["reader_reanalysis"])

    def test_copy_retains_complete_packet_and_does_not_modify_inputs(self):
        with tempfile.TemporaryDirectory() as temp:
            path, _, method, _, observe, _ = fixture(Path(temp) / "source")
            before = {p.relative_to(path.parent): p.read_bytes() for p in path.parent.rglob("*") if p.is_file()}
            dest = Path(temp) / "copy/reference.json"; copy_reference(path, dest)
            self.assertEqual(before, {p.relative_to(dest.parent): p.read_bytes() for p in dest.parent.rglob("*") if p.is_file()})
            self.assertEqual(validate_reference(path, method, observe), validate_reference(dest, method, observe))


if __name__ == "__main__": unittest.main()
