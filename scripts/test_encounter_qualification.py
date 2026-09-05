#!/usr/bin/env python3
"""Adversarial tests for the exact encounter-reader qualification gate."""

import copy
from collections import Counter
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch


sys.path.insert(0, str(Path(__file__).resolve().parent / "bench"))
from encounter_expectation import compare_sample
import encounter_qualification
from encounter_qualification import (
    CLASSIFIER_IMPLEMENTATION_FILES,
    CORE_READER_FILES,
    FIELDS,
    OCR_RUNTIME_FILES,
    QUALIFICATION_LOGIC_FILES,
    REQUIRED_FAULT_CONTROLS,
    TEMPORAL_SOURCE_HASH_FIELDS,
    TEMPORAL_V2_INTEGRITY_CHECKS,
    TEMPORAL_V2_OBSERVER_RUBRICS,
    temporal_v2_observer_instructions,
    verify_qualification,
)


SHA = "a" * 64
BENCH_SHA = "c" * 64
SOURCE_GIT_SHA = "0123456789abcdef0123456789abcdef01234567"
READER = {
    "method_version": 5,
    "ocr": "test",
    "ocr_source_sha256": "b" * 64,
    "ocr_available": True,
    "ocr_compiled": True,
    "ocr_runtime_probe": {"status": "operational", "probe_sha256": SHA},
}
CAMERA = {
    "name": "Global Shutter Camera",
    "profile": {"framerate": 200, "video_size": "1280x720"},
}
CARD = {"band": "Ka", "frequency": "34.700", "direction": "front", "bars": 3}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def canonical_digest(value):
    return hashlib.sha256(json.dumps(
        value, sort_keys=True, separators=(",", ":"), allow_nan=False).encode("utf-8")).hexdigest()


def literal(field):
    values = {
        "counter_glyph": "3",
        "primary_frequency": "34.700",
        "active_bands": ["Ka"],
        "main_arrows": ["front"],
        "main_bars": 3,
        "secondary": [copy.deepcopy(CARD)],
        "muted_badge": False,
    }
    return {"state": "readable", "value": copy.deepcopy(values[field])}


def expected_state():
    values = {field: literal(field)["value"] for field in FIELDS}
    return {
        "fields": {field: {"allowed": [copy.deepcopy(value)]}
                   for field, value in values.items()},
        "joint_states": [{
            "counter_glyph": values["counter_glyph"],
            "active_bands": values["active_bands"],
            "main_arrows": values["main_arrows"],
        }],
        "secondary_policy": {
            "required": values["secondary"],
            "previously_seen": [],
            "retirement_unknown": False,
        },
        "previous_input": None,
    }


def observed_fields():
    return {field: literal(field) for field in FIELDS}


class QualificationTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.root = Path(self.directory.name)
        names = (*CORE_READER_FILES, *OCR_RUNTIME_FILES, *QUALIFICATION_LOGIC_FILES,
                 "encounter_temporal.py", "encounter_arrow_transition.py",
                 "encounter_bar_transition.py", "encounter_mute_redraw_transition.py",
                 "encounter_redraw_probe.py")
        self.method = {name: SHA for name in names}
        self.policy = {"qualified_temporal_classifiers": {}}
        self.reader_observations = {}

    def tearDown(self):
        self.directory.cleanup()

    def write_json(self, relative, document):
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(document, sort_keys=True), encoding="utf-8")
        return path

    def write_image(self, relative, label):
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b"\x89PNG\r\n\x1a\nqualified-test-image:" + label.encode("ascii"))
        return path

    @staticmethod
    def reference(path, root):
        return {"path": str(path.relative_to(root)), "sha256": digest(path)}

    def field_validation(self, *, wrong=False):
        manifest_frames = []
        blind_frames = []
        frames = []
        totals = Counter()
        per_field = {field: Counter() for field in FIELDS}
        for index in range(20):
            frame_id = f"blind-{index:02d}"
            image = self.write_image(f"field/images/{frame_id}.png", frame_id)
            image_sha = digest(image)
            manifest_frames.append({"frame_id": frame_id, "image_sha256": image_sha})
            blind = {"frame_id": frame_id, "image_sha256": image_sha}
            checks = []
            for field in FIELDS:
                reference = literal(field)
                blind[field] = copy.deepcopy(reference)
                observed = copy.deepcopy(reference)
                status = "AGREEMENT"
                if wrong and index == 0 and field == "counter_glyph":
                    observed = {"state": "readable", "value": "8"}
                    status = "WRONG_ASSERTION"
                checks.append({"field": field, "status": status,
                               "observed": observed, "reference": reference})
                totals[status] += 1
                per_field[field][status] += 1
            blind_frames.append(blind)
            self.reader_observations[image_sha] = {
                check["field"]: copy.deepcopy(check["observed"]) for check in checks}
            frames.append({
                "frame_id": frame_id,
                "image": self.reference(image, self.root),
                "image_sha256": image_sha,
                "checks": checks,
            })

        manifest = self.write_json("field/sources/blind-manifest.json",
                                   {"frames": manifest_frames})
        observations = self.write_json("field/sources/blind-observations.json",
                                       {"frames": blind_frames})
        selection = self.write_json("field/sources/selection.json", {
            "selection_rule": "complete reserved frame set",
            "selected_frame_ids": [frame["frame_id"] for frame in manifest_frames],
            "registration": {"result": "PASS", "method": "retained-test-registration"},
        })
        sources = {
            "blind_manifest": self.reference(manifest, self.root),
            "blind_observations": self.reference(observations, self.root),
            "selection": self.reference(selection, self.root),
        }
        return {
            "schema_version": 1,
            "kind": "reserved_independent_reader_validation",
            "reader": copy.deepcopy(READER),
            "camera": copy.deepcopy(CAMERA),
            "method": {
                "method_version": 5,
                "files": {name: self.method[name] for name in CORE_READER_FILES},
                "reference_source_sha256": {
                    "blind-manifest.json": sources["blind_manifest"]["sha256"],
                    "blind-observations.json": sources["blind_observations"]["sha256"],
                    "selection.json": sources["selection"]["sha256"],
                },
            },
            "source_artifacts": sources,
            "unique_original_frames": len(frames),
            "required_field_labels": len(frames) * len(FIELDS),
            "counts": dict(totals),
            "fields": {field: dict(per_field[field]) for field in FIELDS},
            "frames": frames,
        }

    def visible_secondary_validation(self, *, empty=False, wrong=False):
        packet_id = "secondary-packet-test-v1"
        image_integrity = {}
        single_labels = []
        single_hidden = []
        items = []
        counts = Counter()
        for index in range(6):
            source_image = f"opaque-secondary-{index:02d}.png"
            retained = self.write_image(f"secondary/images/{source_image}", source_image)
            image_sha = digest(retained)
            image_integrity[source_image] = {"sha256": image_sha}
            cards = [] if empty else [{
                "band": "Ka",
                "frequency": "34.700",
                "direction": "up",
                "bar_count": 3,
                "meter_cells": ["lit", "lit", "lit", "unlit", "unlit", "unlit"],
            }]
            blind_label = {
                "image": source_image,
                "display_visibility": "visible",
                "card_count": len(cards),
                "cards": cards,
            }
            reference_value = [] if empty else [copy.deepcopy(CARD)]
            reference = {"state": "readable", "value": reference_value}
            observed = copy.deepcopy(reference)
            if wrong and index == 0:
                observed["value"][0]["bars"] = 4
            status = "AGREEMENT" if observed == reference else "WRONG_ASSERTION"
            counts[status] += 1
            self.reader_observations[image_sha] = {"secondary": copy.deepcopy(observed)}
            single_labels.append({"item_id": f"single-{index:02d}", "frame": blind_label})
            single_hidden.append({"item_id": f"single-{index:02d}",
                                  "image": source_image,
                                  "machine_secondary": copy.deepcopy(observed)})
            items.append({
                "item_id": source_image,
                "source_image": source_image,
                "image": self.reference(retained, self.root),
                "image_sha256": image_sha,
                "blind_label": copy.deepcopy(blind_label),
                "observed": observed,
                "primary_reference": copy.deepcopy(reference),
                "reference": reference,
                "status": status,
            })

        packet = self.write_json("secondary/sources/packet-manifest.json", {
            "packet_id": packet_id,
            "image_integrity": image_integrity,
        })
        labels = self.write_json("secondary/sources/blind-labels.json", {
            "packet_id": packet_id,
            "ordered_sequences": [],
            "single_frame_items": single_labels,
        })
        hidden = self.write_json("secondary/sources/sealed-key.json", {
            "packet_id": packet_id,
            "sequences": [],
            "single_frame_controls": single_hidden,
        })
        adjudication = self.write_json("secondary/sources/adjudication.json", {"items": []})
        sources = {
            "packet_manifest": self.reference(packet, self.root),
            "blind_labels": self.reference(labels, self.root),
            "sealed_key": self.reference(hidden, self.root),
            "adjudication": self.reference(adjudication, self.root),
        }
        return {
            "schema_version": 1,
            "kind": "independent_visible_secondary_validation",
            "reader": copy.deepcopy(READER),
            "camera": copy.deepcopy(CAMERA),
            "method": {"method_version": 5,
                       "files": {name: self.method[name] for name in CORE_READER_FILES}},
            "blind_protocol": {
                "labels_completed_before_key_access": True,
                "observer_received_machine_output": False,
                "observer_received_hidden_key": False,
            },
            "source": {
                "packet_manifest_sha256": sources["packet_manifest"]["sha256"],
                "blind_labels_sha256": sources["blind_labels"]["sha256"],
                "sealed_key_sha256": sources["sealed_key"]["sha256"],
            },
            "source_artifacts": sources,
            "registration": {"result": "PASS", "method": "retained-test-registration"},
            "unique_original_frames": len(items),
            "counts": dict(counts),
            "items": items,
        }

    def fault_controls(self, *, broken_control=None):
        expected = expected_state()
        cases = []
        for index, (name, (desired, differences, unresolved)) in enumerate(
                REQUIRED_FAULT_CONTROLS.items()):
            fields = observed_fields()
            if name == "missing_primary":
                fields["primary_frequency"] = {"state": "absent", "value": None}
            elif name == "wrong_strength":
                fields["main_bars"] = {"state": "readable", "value": 4}
            elif name == "wrong_direction":
                fields["main_arrows"] = {"state": "readable", "value": ["rear"]}
            elif name == "stale_primary":
                fields["primary_frequency"] = {"state": "readable", "value": "34.701"}
            elif name == "missing_secondary":
                fields["secondary"] = {"state": "absent", "value": None}
            elif name == "wrong_card_association":
                wrong_card = {**CARD, "frequency": "34.701"}
                fields["secondary"] = {"state": "readable", "value": [wrong_card]}
            elif name == "unreadable_camera":
                fields = {field: {"state": "unreadable", "value": None,
                                  "reason": "camera content unavailable"}
                          for field in FIELDS}
            elif name == "partial_frequency":
                fields["primary_frequency"] = {
                    "state": "unreadable", "value": None, "reason": "partial glyph"}
            elif name == "failure_with_unknown":
                fields["primary_frequency"] = {"state": "readable", "value": "34.701"}
                fields["main_bars"] = {"state": "readable", "value": 4}
                fields["main_arrows"] = {
                    "state": "unreadable", "value": None, "reason": "arrow phase unavailable"}

            if name == broken_control:
                fields = observed_fields()
            observed = fields
            derived = compare_sample(expected, observed, role="held")
            image = self.write_image(f"controls/images/{index:02d}-{name}.png", name)
            self.reader_observations[digest(image)] = copy.deepcopy(observed)
            cases.append({
                "name": name,
                "desired_status": desired,
                "required_differences": list(differences),
                "required_unresolved": list(unresolved),
                "image": self.reference(image, self.root),
                "image_sha256": digest(image),
                "observed": observed,
                "comparison": derived,
                "demonstrated": name != broken_control,
            })
        return {
            "schema_version": 1,
            "kind": "encounter_reader_fault_controls",
            "reader": copy.deepcopy(READER),
            "camera": copy.deepcopy(CAMERA),
            "method": {"method_version": 5,
                       "files": {name: self.method[name] for name in CORE_READER_FILES}},
            "expected": expected,
            "registration": {"result": "PASS", "method": "retained-test-registration"},
            "required": len(cases),
            "demonstrated": sum(case["demonstrated"] for case in cases),
            "cases": cases,
        }

    def temporal_validation(self):
        classifier = "v1-arrow-phase-edge-v2"
        temporal_root = self.root / "temporal"
        spec = self.write_json("temporal/spec.json", {
            "classifier_id": classifier,
            "endpoint_separation_floor": 52,
        })
        spec_sha = digest(spec)
        manifest_items = []
        observations = []
        hidden_items = []
        frozen_records = []
        comparisons = []
        clip_checks = []
        outcome_ids = {name: [] for name in
                       ("true_admit", "false_admit", "true_reject", "false_reject")}
        for index in range(10):
            opaque_id = f"opaque-{index:02d}"
            eligible = index < 5
            decision = "ADMITTED" if eligible else "REJECTED"
            literal_observation = ({
                "center_class": "COHERENT_SINGLE_DIRECTION_ON_OFF_EDGE",
                "endpoint_support": "BOTH_CLEAR",
                "extra_direction_motion": "NO",
                "confidence": "HIGH",
            } if eligible else {
                "center_class": "NO_CLEAR_EDGE",
                "endpoint_support": "ONE_CLEAR",
                "extra_direction_motion": "NO",
                "confidence": "LOW",
            })
            code = None if eligible else "ENDPOINTS_NOT_SEPARATED"
            separation = 64.0 if eligible else 12.0
            indices = [index * 3, index * 3 + 1, index * 3 + 2]
            clip = temporal_root / "sources" / "clips" / f"{opaque_id}.bin"
            clip.parent.mkdir(parents=True, exist_ok=True)
            clip.write_bytes(f"qualified temporal clip {opaque_id}".encode("ascii"))
            clip_sha = digest(clip)
            size = clip.stat().st_size
            manifest_items.append({
                "opaque_id": opaque_id,
                "clip": f"clips/{opaque_id}.bin",
                "sha256": clip_sha,
                "size_bytes": size,
            })
            observations.append({"opaque_id": opaque_id, **literal_observation})
            record = {"code": code, "endpoint_separation_rms": separation}
            hidden_items.append({
                "opaque_id": opaque_id,
                "clip_sha256": clip_sha,
                "frozen_classifier_decision": decision,
                "frozen_classifier_record": record,
                "target_ambiguous_run_video_indices": [indices[0], indices[-1]],
            })
            frozen_records.append({"opaque_id": opaque_id, "decision": decision,
                                   "record": record})
            outcome = "TRUE_ADMIT" if eligible else "TRUE_REJECT"
            outcome_ids[outcome.casefold()].append(opaque_id)
            comparisons.append({
                "opaque_id": opaque_id,
                "observer_literal": literal_observation,
                "observer_strict_visual_eligible_for_admission": eligible,
                "frozen_classifier_decision": decision,
                "frozen_rejection_code": code,
                "endpoint_separation_rms": separation,
                "source_video_frame_indices": indices,
                "comparison_outcome": outcome,
            })
            clip_checks.append({
                "opaque_id": opaque_id,
                "actual_sha256": clip_sha,
                "hidden_key_sha256": clip_sha,
                "sealed_sha256": clip_sha,
                "actual_size_bytes": size,
                "sealed_size_bytes": size,
                "path_matches_id": True,
                "source_frame_count_matches_range": True,
                "target_run_inside_clip": True,
            })

        source_paths = {}
        source_paths["observer_manifest"] = self.write_json(
            "temporal/sources/observer-manifest.json", {
                "item_count": len(manifest_items), "items": manifest_items})
        readme = temporal_root / "sources" / "observer-readme.txt"
        readme.write_text("Blindly inspect every opaque clip before key access.\n", encoding="utf-8")
        source_paths["observer_readme"] = readme
        source_paths["completed_observations"] = self.write_json(
            "temporal/sources/completed-observations.json", {
                "instructions_sha256": digest(readme),
                "observations": observations,
            })
        source_paths["pre_pixel_freeze"] = self.write_json(
            "temporal/sources/pre-pixel-freeze.json", {
                "allowlist_status": "NOT_ALLOWLISTED",
                "classifier": {"id": classifier, "spec_sha256": spec_sha},
            })
        source_paths["restricted_hidden_key"] = self.write_json(
            "temporal/sources/restricted-hidden-key.json", {
                "do_not_provide_to_observer": True,
                "classifier_id": classifier,
                "classifier_spec_sha256": spec_sha,
                "allowlist_status": "NOT_ALLOWLISTED_PENDING_BLIND_ADJUDICATION",
                "items": hidden_items,
            })
        source_paths["selection"] = self.write_json(
            "temporal/sources/selection.json", {
                "selection_rule": "all frozen candidates",
                "opaque_ids": [item["opaque_id"] for item in manifest_items],
            })
        source_paths["frozen_classifier_result"] = self.write_json(
            "temporal/sources/frozen-result.json", {
                "classifier_id": classifier,
                "classifier_spec_sha256": spec_sha,
                "classifier_source_sha256": self.method["encounter_arrow_transition.py"],
                "reader_source_sha256": self.method["encounter_reader.py"],
                "pre_pixel_freeze_sha256": digest(source_paths["pre_pixel_freeze"]),
                "selection_sha256": digest(source_paths["selection"]),
                "classifications": [
                    item["frozen_classifier_record"] for item in hidden_items
                    if item["frozen_classifier_decision"] == "ADMITTED"],
                "rejected_runs": [
                    item["frozen_classifier_record"] for item in hidden_items
                    if item["frozen_classifier_decision"] == "REJECTED"],
                "errors": [],
            })
        relative_refs = {
            name: {"path": str(path.relative_to(temporal_root)), "sha256": digest(path)}
            for name, path in source_paths.items()
        }
        seal_document = {
            "classifier_spec_sha256": spec_sha,
            "classifier_source_sha256": self.method["encounter_arrow_transition.py"],
            "reader_source_sha256": self.method["encounter_reader.py"],
            "allowlist_status": "NOT_ALLOWLISTED",
        }
        for name in ("pre_pixel_freeze", "frozen_classifier_result", "observer_manifest",
                     "observer_readme", "restricted_hidden_key", "selection"):
            seal_document[TEMPORAL_SOURCE_HASH_FIELDS[name]] = relative_refs[name]["sha256"]
        source_paths["seal"] = self.write_json("temporal/sources/seal.json", seal_document)
        relative_refs["seal"] = {
            "path": str(source_paths["seal"].relative_to(temporal_root)),
            "sha256": digest(source_paths["seal"]),
        }

        matrix = {name: len(ids) for name, ids in outcome_ids.items()}
        matrix["total"] = len(comparisons)
        admissions = matrix["true_admit"] + matrix["false_admit"]
        rejections = matrix["true_reject"] + matrix["false_reject"]
        positives = matrix["true_admit"] + matrix["false_reject"]
        negatives = matrix["true_reject"] + matrix["false_admit"]
        comparison_document = {
            "schema_version": 1,
            "classifier_id": classifier,
            "classifier_spec_sha256": spec_sha,
            "source_artifacts": {
                TEMPORAL_SOURCE_HASH_FIELDS[name]: reference["sha256"]
                for name, reference in relative_refs.items()
            },
            "comparisons": comparisons,
            "integrity": {
                "checks": {"blind_ids_match": True, "clips_match_seal": True},
                "clip_checks": clip_checks,
            },
            "confusion_matrix": matrix,
            **{f"{name}_ids": ids for name, ids in outcome_ids.items()},
            "denominators": {
                "classifier_admissions": admissions,
                "classifier_rejections": rejections,
                "observer_visual_positives": positives,
                "observer_visual_negatives_or_uncertain": negatives,
                "false_admit": {
                    "count": matrix["false_admit"],
                    "denominator_classifier_admissions": admissions,
                    "rate": matrix["false_admit"] / admissions if admissions else None,
                },
                "false_reject": {
                    "count": matrix["false_reject"],
                    "denominator_observer_visual_positives": positives,
                    "rate": matrix["false_reject"] / positives if positives else None,
                },
            },
            "rates": {
                "accuracy": (matrix["true_admit"] + matrix["true_reject"]) / matrix["total"],
                "precision": matrix["true_admit"] / admissions if admissions else None,
                "recall": matrix["true_admit"] / positives if positives else None,
                "specificity": matrix["true_reject"] / negatives if negatives else None,
            },
            "numerical_minima": {
                "required_true_admit_minimum": 5,
                "required_true_reject_minimum": 5,
                "observed_true_admit": matrix["true_admit"],
                "observed_true_reject": matrix["true_reject"],
                "true_admit_minimum_met": True,
                "true_reject_minimum_met": True,
            },
            "integrity_pass": True,
            "allowlist_decision": {"allowlist_exact_classifier": True},
        }
        comparison_path = self.write_json("temporal/comparison.json", comparison_document)
        entry = {
            "classifier_spec_sha256": spec_sha,
            "spec": self.reference(spec, self.root),
            "validation": self.reference(comparison_path, self.root),
            "source_artifacts": relative_refs,
        }
        self.policy = {"qualified_temporal_classifiers": {
            classifier: {"classifier_spec_sha256": spec_sha}}}
        return {classifier: entry}, {
            "classifier": classifier,
            "comparison": comparison_path,
            "source_paths": source_paths,
            "clips": [temporal_root / "sources" / item["clip"] for item in manifest_items],
        }

    @staticmethod
    def generic_temporal_literal(classifier, eligible, *, indeterminate=False):
        if classifier == "v1-main-bar-adjacent-redraw-v1":
            return ({
                "left_endpoint_bar_count": 2,
                "right_endpoint_bar_count": 3,
                "endpoint_support": "BOTH_CLEAR",
                "transition_class": "COHERENT_SINGLE_BOUNDARY_ADJACENT_REDRAW",
                "extra_cell_motion": "NO",
                "direction": "RISING",
                "confidence": "HIGH",
            } if eligible else ({
                "left_endpoint_bar_count": None,
                "right_endpoint_bar_count": None,
                "endpoint_support": "INDETERMINATE",
                "transition_class": "VISUALLY_INDETERMINATE",
                "extra_cell_motion": "INDETERMINATE",
                "direction": "INDETERMINATE",
                "confidence": "LOW",
            } if indeterminate else {
                "left_endpoint_bar_count": 2,
                "right_endpoint_bar_count": 4,
                "endpoint_support": "BOTH_CLEAR",
                "transition_class": "MULTIPLE_OR_NONADJACENT_BAR_CHANGE",
                "extra_cell_motion": "YES",
                "direction": "RISING",
                "confidence": "HIGH",
            }))
        if classifier == "v1-muted-badge-rising-fill-v1":
            return ({
                "left_endpoint_badge": "ABSENT",
                "right_endpoint_badge": "PRESENT",
                "endpoint_support": "BOTH_CLEAR",
                "transition_class": "COHERENT_BADGE_RISING_FILL",
                "direction": "RISING",
                "outside_badge_content_change": "NO",
                "confidence": "HIGH",
            } if eligible else ({
                "left_endpoint_badge": "INDETERMINATE",
                "right_endpoint_badge": "INDETERMINATE",
                "endpoint_support": "INDETERMINATE",
                "transition_class": "VISUALLY_INDETERMINATE",
                "direction": "INDETERMINATE",
                "outside_badge_content_change": "INDETERMINATE",
                "confidence": "LOW",
            } if indeterminate else {
                "left_endpoint_badge": "PRESENT",
                "right_endpoint_badge": "ABSENT",
                "endpoint_support": "BOTH_CLEAR",
                "transition_class": "BADGE_FALLING_OR_NONMONOTONE",
                "direction": "FALLING",
                "outside_badge_content_change": "NO",
                "confidence": "HIGH",
            }))
        if classifier == "v1-unmute-stable-frequency-sweep-v1":
            return ({
                "left_endpoint_frequency": "34.700",
                "right_endpoint_frequency": "34.700",
                "endpoint_support": "BOTH_CLEAR",
                "transition_class": "COHERENT_STABLE_FREQUENCY_ILLUMINATION_SWEEP",
                "glyph_relation": "SAME_GLYPHS",
                "illumination_direction": "DIM_TO_BRIGHT",
                "extra_ink_or_hole_fill": "NO",
                "confidence": "HIGH",
            } if eligible else ({
                "left_endpoint_frequency": None,
                "right_endpoint_frequency": None,
                "endpoint_support": "INDETERMINATE",
                "transition_class": "VISUALLY_INDETERMINATE",
                "glyph_relation": "INDETERMINATE",
                "illumination_direction": "INDETERMINATE",
                "extra_ink_or_hole_fill": "INDETERMINATE",
                "confidence": "LOW",
            } if indeterminate else {
                "left_endpoint_frequency": "34.700",
                "right_endpoint_frequency": "35.500",
                "endpoint_support": "BOTH_CLEAR",
                "transition_class": "GLYPH_CHANGE_OR_EXTRA_STROKE",
                "glyph_relation": "DIFFERENT_GLYPHS",
                "illumination_direction": "NONMONOTONE",
                "extra_ink_or_hole_fill": "YES",
                "confidence": "HIGH",
            }))
        raise AssertionError(f"no test rubric for {classifier}")

    def generic_temporal_validation(self, classifier, *, schema_version=2,
                                    binding_tamper=None, binding_file=None,
                                    reader_tamper=None, rubric_tamper=None,
                                    selection_tamper=None, false_admit=False,
                                    claim_mismatch=False, duplicate_candidate=False,
                                    contradictory_spec=False,
                                    blind_protocol_tamper=False,
                                    malformed_admitted_record=False,
                                    clip_path_swap=False, empty_readme=False,
                                    record_tamper=None, rejection_code="UNCLOSED_RUN",
                                    clip_mapping_tamper=None,
                                    indeterminate_rejects=False):
        temporal_root = self.root / f"temporal-v2-{classifier}"
        repository_spec = (Path(__file__).resolve().parent / "bench" / "temporal_specs" /
                           f"{classifier}.json")
        spec_document = json.loads(repository_spec.read_text(encoding="utf-8"))
        spec_document["identity"] = {
            "reader_method_version": READER["method_version"],
            "reader_sha256": self.method["encounter_reader.py"],
            "redraw_probe_method_version": 1,
            "redraw_probe_sha256": self.method["encounter_redraw_probe.py"],
        }
        if contradictory_spec:
            spec_document["validation"].update(
                minimum_blind_true_admits=50,
                minimum_blind_true_rejects=50,
                observer_eligibility_rule="NEVER_ELIGIBLE")
        spec = self.write_json(f"{temporal_root.name}/spec.json", spec_document)
        spec_sha = digest(spec)
        implementation_binding = {
            name: self.method[name] for name in CLASSIFIER_IMPLEMENTATION_FILES[classifier]}
        reader_binding = {
            "method_version": READER["method_version"],
            "source_sha256": self.method["encounter_reader.py"],
            "runtime_sha256": canonical_digest(READER),
            "bench_source_sha256": BENCH_SHA,
        }
        rubric_sha = canonical_digest(TEMPORAL_V2_OBSERVER_RUBRICS[classifier])

        def retained_reader(artifact):
            value = copy.deepcopy(reader_binding)
            if reader_tamper == artifact:
                value["runtime_sha256"] = "b" * 64
            return value

        def retained_rubric(artifact):
            return "b" * 64 if rubric_tamper == artifact else rubric_sha

        manifest_items = []
        observations = []
        hidden_items = []
        admitted_records = []
        rejected_records = []
        comparisons = []
        clip_checks = []
        outcome_ids = {name: [] for name in
                       ("true_admit", "false_admit", "true_reject", "false_reject",
                        "abstain")}
        ground_truth_counts = Counter()
        item_count = 11 if false_admit or claim_mismatch else 10
        affected = TEMPORAL_V2_OBSERVER_RUBRICS[classifier]["raw_affected_fields"]
        selection_document = {
            "schema_version": 2,
            "kind": "blind_temporal_classifier_selection",
            "selection_rule": "ALL_FROZEN_CANDIDATES",
            "opaque_ids": [f"opaque-{index:02d}" for index in range(item_count)],
        }
        if selection_tamper == "reordered":
            selection_document["opaque_ids"].reverse()
        selection_path = self.write_json(
            f"{temporal_root.name}/sources/selection.json",
            [] if selection_tamper == "wrong_type" else selection_document)
        capture_id = "d" * 64
        maximum_source_interval_ns = 5_000_000
        analysis_indices = sorted({value for index in range(item_count)
                                   for value in (9 + index * 3, 10 + index * 3,
                                                 11 + index * 3, 12 + index * 3)})
        analysis_selection_path = self.write_json(
            f"{temporal_root.name}/sources/analysis-selection.json", {
                "schema_version": 1,
                "identity": {"capture_id": capture_id},
                "selection_mode": "qualification_temporal_candidates",
                "samples": [{"video_frame_index": value,
                             "source_frame_seq": 1000 + value,
                             "capture_ns": 1_000_000_000 +
                                 value * maximum_source_interval_ns}
                            for value in analysis_indices],
            })
        record_context = {
            "capture_id": capture_id,
            "selection_manifest_sha256": digest(analysis_selection_path),
            "verified_maximum_source_interval_ns": maximum_source_interval_ns,
            "reader_method_version": READER["method_version"],
            "reader_sha256": self.method["encounter_reader.py"],
            "redraw_probe_method_version": 1,
            "redraw_probe_sha256": self.method["encounter_redraw_probe.py"],
        }

        def point(video_index):
            return {
                "video_frame_index": video_index,
                "source_frame_seq": 1000 + video_index,
                "capture_ns": 1_000_000_000 + video_index * maximum_source_interval_ns,
            }

        for index in range(item_count):
            opaque_id = f"opaque-{index:02d}"
            claim_mismatch_item = claim_mismatch and index == 10
            visually_eligible = index < 5 or claim_mismatch_item
            machine_admitted = visually_eligible or (false_admit and index == 10)
            decision = "ADMITTED" if machine_admitted else "REJECTED"
            literal_observation = self.generic_temporal_literal(
                classifier, visually_eligible,
                indeterminate=indeterminate_rejects and not visually_eligible)
            _, ground_truth = encounter_qualification._temporal_v2_observer_ground_truth(
                classifier, {"opaque_id": opaque_id, **literal_observation})
            ground_truth_counts[ground_truth] += 1
            indices = [10 + index * 3, 11 + index * 3, 12 + index * 3]
            if duplicate_candidate and index == 1:
                indices = [10, 11, 12]
            event_id = "event-0000" if duplicate_candidate and index == 1 else f"event-{index:04d}"
            full_indices = ([indices[0] - 1, *indices]
                            if classifier == "v1-unmute-stable-frequency-sweep-v1"
                            and machine_admitted else list(indices))
            clip = temporal_root / "sources" / "clips" / f"{opaque_id}.bin"
            clip.parent.mkdir(parents=True, exist_ok=True)
            clip.write_bytes(f"qualified generic temporal clip {opaque_id}".encode("ascii"))
            clip_sha = digest(clip)
            size = clip.stat().st_size
            clip_source_indices = list(range(max(0, indices[0] - 10), indices[-1] + 11))
            target_clip_indices = [clip_source_indices.index(value) for value in indices]
            if clip_mapping_tamper == "source_gap" and index == 0:
                clip_source_indices[1] += 1
            if clip_mapping_tamper == "target_position" and index == 0:
                target_clip_indices[0] += 1
            manifest_items.append({
                "opaque_id": opaque_id,
                "clip": f"clips/{opaque_id}.bin",
                "sha256": clip_sha,
                "size_bytes": size,
                "target_run_video_indices": indices,
                "clip_source_video_indices": clip_source_indices,
                "target_run_clip_frame_indices": target_clip_indices,
                "full_run_video_indices": full_indices,
                "full_run_clip_frame_indices": [
                    clip_source_indices.index(value) for value in full_indices],
                "inset_source_box": [10, 10, 20, 20],
            })
            observations.append({"opaque_id": opaque_id, **literal_observation})
            if machine_admitted:
                record = {
                    "event_id": event_id,
                    "classifier_id": classifier,
                    "classifier_spec_sha256": spec_sha,
                    "status": "QUALIFIED_CAPTURE_TRANSITION",
                    "raw_affected_fields": copy.deepcopy(affected),
                    "video_frame_indices": indices,
                    "first": point(indices[0]),
                    "last": point(indices[-1]),
                    **record_context,
                    "basis": "Synthetic exact-shape qualification control.",
                }
                if malformed_admitted_record and index == 0:
                    record["raw_affected_fields"] = []
                if classifier == "v1-main-bar-adjacent-redraw-v1":
                    profile = spec_document["profile"]
                    constants = spec_document["constants"]
                    record.update({
                        "left_support": point(indices[0] - 2),
                        "left_endpoint": point(indices[0] - 1),
                        "right_endpoint": point(indices[-1] + 1),
                        "right_support": point(indices[-1] + 2),
                        "endpoint_values": [2, 3],
                        "changed_bar_index": 2,
                        "main_bar_expectation_signature": {
                            "previous_count": 2, "current_count": 3},
                        "endpoint_separation_rms": 30.0,
                        "projections": [0.25, 0.5, 0.75],
                        "normalized_residuals": [0.01, 0.01, 0.01],
                        "maximum_backward_step": 0.0,
                        "total_backward_motion": 0.0,
                        "boundary_medians": [10.0, 20.0, 30.0, 40.0, 50.0],
                        "maximum_boundary_median_backward_step": 0.0,
                        "unchanged_cell_profile_diameter_rms": {
                            str(cell): 1.0 for cell in range(6) if cell != 2},
                        "maximum_endpoint_span_ns":
                            constants["authored_display_update_ns"] +
                            maximum_source_interval_ns,
                        "profile_schema": {
                            "rows": profile["rows"], "columns": profile["columns"],
                            "sample": profile["sample"]},
                        "profile_boxes": profile["bar_boxes_bottom_to_top"],
                    })
                elif classifier == "v1-muted-badge-rising-fill-v1":
                    stable_fields = copy.deepcopy(expected_state()["fields"])
                    stable_fields.pop("muted_badge")
                    signature = {
                        "previous_muted_badge": False, "target_muted_badge": True,
                        "stable_primary_frequency": "34.700", "stable_fields": stable_fields,
                        "joint_states": copy.deepcopy(expected_state()["joint_states"]),
                    }
                    component_count = spec_document["profile"]["cells"]
                    progress = [[step / 6] * component_count for step in range(7)]
                    record.update({
                        "left_support": [point(indices[0] - 2), point(indices[0] - 1)],
                        "right_support": [point(indices[-1] + 1), point(indices[-1] + 2)],
                        "event_signature": signature,
                        "endpoint_component_separation": [30.0] * component_count,
                        "component_progress": progress,
                        "maximum_backward_step": 0.0,
                        "total_backward_motion_by_component": [0.0] * component_count,
                        "maximum_support_chain_span_ns":
                            spec_document["constants"]["maximum_support_chain_span_ns"],
                    })
                else:
                    stable_frequency = "35.500" if claim_mismatch_item else "34.700"
                    stable_fields = copy.deepcopy(expected_state()["fields"])
                    stable_fields.pop("muted_badge")
                    stable_fields["primary_frequency"] = {"allowed": [stable_frequency]}
                    signature = {
                        "previous_muted_badge": True, "target_muted_badge": False,
                        "stable_primary_frequency": stable_frequency,
                        "stable_fields": stable_fields,
                        "joint_states": copy.deepcopy(expected_state()["joint_states"]),
                    }
                    masks = [{
                        "0": "abcdef", "1": "bc", "2": "abdeg", "3": "abcdg",
                        "4": "bcfg", "5": "acdfg", "6": "acdefg", "7": "abc",
                        "8": "abcdefg", "9": "abcdfg",
                    }[digit] for digit in stable_frequency.replace(".", "")]
                    component_count = sum(len(mask) for mask in masks)
                    progress = [[step / (len(full_indices) + 3)] * component_count
                                for step in range(len(full_indices) + 4)]
                    record.update({
                        "full_field_run_indices": full_indices,
                        "full_field_run_first": point(full_indices[0]),
                        "full_field_run_last": point(full_indices[-1]),
                        "left_support": [point(full_indices[0] - 2),
                                         point(full_indices[0] - 1)],
                        "right_support": [point(full_indices[-1] + 1),
                                          point(full_indices[-1] + 2)],
                        "event_signature": signature,
                        "expected_digit_masks": masks,
                        "endpoint_component_separation": [60.0] * component_count,
                        "component_progress": progress,
                        "maximum_backward_step": 0.0,
                        "total_backward_motion_by_component": [0.0] * component_count,
                        "maximum_support_chain_span_ns":
                            spec_document["constants"]["maximum_support_chain_span_ns"],
                    })
                if index == 0:
                    if record_tamper == "bar_changed_index":
                        record["changed_bar_index"] = 5
                    elif record_tamper == "bar_expectation":
                        record["main_bar_expectation_signature"] = {
                            "previous_count": 6, "current_count": 0}
                    elif record_tamper == "badge_direction":
                        record["event_signature"]["previous_muted_badge"] = True
                    elif record_tamper == "frequency_masks":
                        record["expected_digit_masks"][0] = "abcdefg"
                    elif record_tamper == "stable_fields":
                        record["event_signature"]["stable_fields"][
                            "primary_frequency"]["allowed"] = ["35.500"]
                    elif record_tamper == "support_sequence":
                        record["left_support"][0]["source_frame_seq"] += 7
                    elif record_tamper == "support_timestamp":
                        record["right_support"][1]["capture_ns"] += 1
                    elif record_tamper == "reader_binding":
                        record["reader_sha256"] = "b" * 64
                    elif record_tamper == "selection_binding":
                        record["selection_manifest_sha256"] = digest(selection_path)
                    elif record_tamper == "extra_field":
                        record["contradictory_extra_field"] = True
                admitted_records.append(record)
                claim_matches = visually_eligible and not claim_mismatch_item
                code = None
            else:
                record = {
                    "event_id": event_id,
                    "field": affected[0],
                    "code": rejection_code,
                    "first": point(indices[0]),
                    "last": point(indices[-1]),
                    "reason": "Synthetic rejected qualification control.",
                }
                if record_tamper == "rejected_field" and index == 5:
                    record["field"] = "invented_field"
                rejected_records.append(record)
                claim_matches = None
                code = rejection_code
            record_sha = canonical_digest(record)
            hidden_items.append({
                "opaque_id": opaque_id,
                "clip_sha256": clip_sha,
                "frozen_classifier_decision": decision,
                "frozen_classifier_record": record,
                "frozen_classifier_record_sha256": record_sha,
                "target_run_video_indices": indices,
                "clip_source_video_indices": clip_source_indices,
                "target_run_clip_frame_indices": target_clip_indices,
                "full_run_video_indices": full_indices,
                "full_run_clip_frame_indices": [
                    clip_source_indices.index(value) for value in full_indices],
            })
            if machine_admitted:
                outcome = ("TRUE_ADMIT" if visually_eligible and claim_matches
                           else "FALSE_ADMIT")
            elif visually_eligible:
                outcome = "FALSE_REJECT"
            elif ground_truth == "DEFINITE_NEGATIVE":
                outcome = "TRUE_REJECT"
            else:
                outcome = "ABSTAIN"
            outcome_ids[outcome.casefold()].append(opaque_id)
            comparisons.append({
                "opaque_id": opaque_id,
                "observer_literal": literal_observation,
                "observer_strict_visual_eligible_for_admission": visually_eligible,
                "observer_ground_truth": ground_truth,
                "observer_claim_matches_frozen_record": claim_matches,
                "frozen_classifier_decision": decision,
                "frozen_rejection_code": code,
                "frozen_classifier_record_sha256": record_sha,
                "source_video_frame_indices": indices,
                "comparison_outcome": outcome,
            })
            clip_checks.append({
                "opaque_id": opaque_id,
                "actual_sha256": clip_sha,
                "hidden_key_sha256": clip_sha,
                "sealed_sha256": clip_sha,
                "actual_size_bytes": size,
                "sealed_size_bytes": size,
                "target_run_video_indices": indices,
                "clip_source_video_indices": clip_source_indices,
                "target_run_clip_frame_indices": target_clip_indices,
                "full_run_video_indices": full_indices,
                "full_run_clip_frame_indices": [
                    clip_source_indices.index(value) for value in full_indices],
                "path_matches_id": True,
                "source_frame_indices_match_target_run": True,
                "target_run_inside_clip": True,
            })

        if clip_path_swap:
            for name in ("clip", "sha256", "size_bytes"):
                manifest_items[0][name], manifest_items[1][name] = (
                    manifest_items[1][name], manifest_items[0][name])
            for index in (0, 1):
                hidden_items[index]["clip_sha256"] = manifest_items[index]["sha256"]
                clip_checks[index].update(
                    actual_sha256=manifest_items[index]["sha256"],
                    hidden_key_sha256=manifest_items[index]["sha256"],
                    sealed_sha256=manifest_items[index]["sha256"],
                    actual_size_bytes=manifest_items[index]["size_bytes"],
                    sealed_size_bytes=manifest_items[index]["size_bytes"])

        source_paths = {}
        source_paths["observer_manifest"] = self.write_json(
            f"{temporal_root.name}/sources/observer-manifest.json", {
                "schema_version": 2,
                "kind": "blind_temporal_observer_manifest",
                "classifier_id": classifier,
                "classifier_spec_sha256": spec_sha,
                "observer_rubric_sha256": retained_rubric("observer_manifest"),
                "blind_protocol": {
                    "observations_completed_before_key_access": True,
                    "observer_received_machine_output": False,
                    "observer_received_hidden_key": False,
                },
                "item_count": len(manifest_items),
                "items": manifest_items,
            })
        readme = temporal_root / "sources" / "observer-readme.txt"
        readme.write_text("" if empty_readme else temporal_v2_observer_instructions(classifier),
                          encoding="utf-8")
        source_paths["observer_readme"] = readme
        source_paths["completed_observations"] = self.write_json(
            f"{temporal_root.name}/sources/completed-observations.json", {
                "instructions_sha256": digest(readme),
                "observer_rubric_sha256": retained_rubric("completed_observations"),
                "blind_protocol": {
                    "observations_completed_before_key_access": True,
                    "observer_received_machine_output": blind_protocol_tamper,
                    "observer_received_hidden_key": False,
                },
                "observations": observations,
            })
        pre_binding = copy.deepcopy(implementation_binding)
        if binding_tamper == "pre_pixel_freeze":
            pre_binding[binding_file or next(iter(pre_binding))] = "b" * 64
        source_paths["pre_pixel_freeze"] = self.write_json(
            f"{temporal_root.name}/sources/pre-pixel-freeze.json", {
                "allowlist_status": "NOT_ALLOWLISTED",
                "classifier": {
                    "id": classifier,
                    "spec_sha256": spec_sha,
                    "implementation_sha256": pre_binding,
                },
                "reader_binding": retained_reader("pre_pixel_freeze"),
                "observer_rubric_sha256": retained_rubric("pre_pixel_freeze"),
            })
        source_paths["restricted_hidden_key"] = self.write_json(
            f"{temporal_root.name}/sources/restricted-hidden-key.json", {
                "do_not_provide_to_observer": True,
                "classifier_id": classifier,
                "classifier_spec_sha256": spec_sha,
                "allowlist_status": "NOT_ALLOWLISTED_PENDING_BLIND_ADJUDICATION",
                "items": hidden_items,
            })
        source_paths["selection"] = selection_path
        source_paths["analysis_selection"] = analysis_selection_path
        source_paths["window_result"] = self.write_json(
            f"{temporal_root.name}/sources/window_result.json", {
                "schema_version": 5,
                "suite": "replay",
                "git_worktree_clean": True,
                "result": "FAIL" if reader_tamper == "capture_window" else "PASS",
                "git_sha": SOURCE_GIT_SHA,
                "runtime_identity": {"git_sha": SOURCE_GIT_SHA[:7]},
                "runtime_qualification": {"status": "qualified", "git_match": True},
                "camera": {
                    "result": "CAPTURED",
                    "capture_id": capture_id,
                    "video_timing_verification_result": {
                        "status": "verified",
                        "maximum_source_interval_ns": maximum_source_interval_ns,
                    },
                },
            })
        capture_source_sha = ("f" * 40 if reader_tamper == "capture_source" else SOURCE_GIT_SHA)
        capture_bench_sha = ("b" * 64 if reader_tamper == "capture_bench" else BENCH_SHA)
        capture_executed = (["encounter_check"]
                            if reader_tamper == "capture_pixels" else [])
        source_paths["qualification_capture"] = self.write_json(
            f"{temporal_root.name}/sources/qualification_capture.json", {
                "schema_version": 1,
                "kind": "blind_visible_reader_qualification_capture",
                "capture_mode": "--qualification-capture",
                "source_git_sha": capture_source_sha,
                "bench_source_sha256": capture_bench_sha,
                "collection": {
                    "result": "PASS",
                    "camera_result": "CAPTURED",
                    "window_result": "window_result.json",
                    "window_result_sha256": digest(source_paths["window_result"]),
                },
                "pixel_analysis": {
                    "status": "WITHHELD_BY_CAPTURE_MODE",
                    "executed": capture_executed,
                    "disabled": ["counter_check", "encounter_check"],
                    "analyzer_outputs_present": False,
                },
                "visible_product_eligible": False,
            })
        source_paths["capture_manifest"] = self.write_json(
            f"{temporal_root.name}/sources/capture/capture_manifest.json",
            {"synthetic_media_fixture": True})
        source_paths["qualification_video"] = (
            temporal_root / "sources/capture/camera.mov")
        source_paths["qualification_video"].write_bytes(b"synthetic retained video")
        source_paths["frame_timing"] = temporal_root / "sources/capture/frame_timing.ndjson"
        source_paths["frame_timing"].write_text("synthetic retained sidecar\n", encoding="utf-8")
        source_paths["video_timing_verification"] = self.write_json(
            f"{temporal_root.name}/sources/capture/video_timing_verification.json",
            {"synthetic_media_fixture": True})
        frozen_binding = copy.deepcopy(implementation_binding)
        if binding_tamper == "frozen_classifier_result":
            frozen_binding[binding_file or next(iter(frozen_binding))] = "b" * 64
        source_paths["frozen_classifier_result"] = self.write_json(
            f"{temporal_root.name}/sources/frozen-result.json", {
                "classifier_id": classifier,
                "classifier_spec_sha256": spec_sha,
                "classifier_implementation_sha256": frozen_binding,
                "reader_binding": retained_reader("frozen_classifier_result"),
                "observer_rubric_sha256": retained_rubric("frozen_classifier_result"),
                "classifier_context": record_context,
                "pre_pixel_freeze_sha256": digest(source_paths["pre_pixel_freeze"]),
                "selection_sha256": digest(source_paths["selection"]),
                "classifications": admitted_records,
                "rejected_runs": rejected_records,
                "errors": [],
            })
        source_paths["analysis_result"] = self.write_json(
            f"{temporal_root.name}/sources/analysis-result.json", {
                "evidence": {"selection_manifest_sha256": digest(analysis_selection_path)},
                "temporal_classification": {
                    "schema_version": 1,
                    "classifications": ([] if record_tamper == "analysis_result" else
                                        copy.deepcopy(admitted_records)),
                    "rejected_runs": copy.deepcopy(rejected_records),
                    "errors": [],
                },
            })
        relative_refs = {
            name: {"path": str(path.relative_to(temporal_root)), "sha256": digest(path)}
            for name, path in source_paths.items()
        }
        seal_binding = copy.deepcopy(implementation_binding)
        if binding_tamper == "seal":
            seal_binding[binding_file or next(iter(seal_binding))] = "b" * 64
        seal_document = {
            "classifier_id": classifier,
            "classifier_spec_sha256": spec_sha,
            "classifier_implementation_sha256": seal_binding,
            "reader_binding": retained_reader("seal"),
            "observer_rubric_sha256": retained_rubric("seal"),
            "allowlist_status": "NOT_ALLOWLISTED",
        }
        for name in ("pre_pixel_freeze", "frozen_classifier_result", "observer_manifest",
                     "observer_readme", "restricted_hidden_key", "selection",
                     "analysis_selection", "qualification_capture", "window_result",
                     "capture_manifest", "qualification_video", "frame_timing",
                     "video_timing_verification", "analysis_result"):
            seal_document[TEMPORAL_SOURCE_HASH_FIELDS[name]] = relative_refs[name]["sha256"]
        source_paths["seal"] = self.write_json(
            f"{temporal_root.name}/sources/seal.json", seal_document)
        relative_refs["seal"] = {
            "path": str(source_paths["seal"].relative_to(temporal_root)),
            "sha256": digest(source_paths["seal"]),
        }

        matrix = {name: len(ids) for name, ids in outcome_ids.items()}
        matrix["total"] = len(comparisons)
        admissions = matrix["true_admit"] + matrix["false_admit"]
        rejections = matrix["true_reject"] + matrix["false_reject"] + matrix["abstain"]
        positives = matrix["true_admit"] + matrix["false_reject"]
        definite_negatives = ground_truth_counts["DEFINITE_NEGATIVE"]
        indeterminate = ground_truth_counts["INDETERMINATE"]
        negatives_or_uncertain = definite_negatives + indeterminate
        scored = matrix["total"] - matrix["abstain"]
        allowed = (matrix["false_admit"] == 0 and matrix["true_admit"] >= 5
                   and matrix["true_reject"] >= 5)
        comparison_document = {
            "schema_version": schema_version,
            "classifier_id": classifier,
            "classifier_spec_sha256": spec_sha,
            "source_artifacts": {
                TEMPORAL_SOURCE_HASH_FIELDS[name]: reference["sha256"]
                for name, reference in relative_refs.items()
            },
            "comparisons": comparisons,
            "integrity": {
                "checks": {name: True for name in TEMPORAL_V2_INTEGRITY_CHECKS},
                "clip_checks": clip_checks,
            },
            "confusion_matrix": matrix,
            **{f"{name}_ids": ids for name, ids in outcome_ids.items()},
            "denominators": {
                "classifier_admissions": admissions,
                "classifier_rejections": rejections,
                "observer_visual_positives": positives,
                "observer_definite_negatives": definite_negatives,
                "observer_indeterminate": indeterminate,
                "observer_visual_negatives_or_uncertain": negatives_or_uncertain,
                "scored_observations": scored,
                "false_admit": {
                    "count": matrix["false_admit"],
                    "denominator_classifier_admissions": admissions,
                    "rate": matrix["false_admit"] / admissions if admissions else None,
                },
                "false_reject": {
                    "count": matrix["false_reject"],
                    "denominator_observer_visual_positives": positives,
                    "rate": matrix["false_reject"] / positives if positives else None,
                },
            },
            "rates": {
                "accuracy": ((matrix["true_admit"] + matrix["true_reject"]) / scored
                             if scored else None),
                "precision": matrix["true_admit"] / admissions if admissions else None,
                "recall": matrix["true_admit"] / positives if positives else None,
                "specificity": (matrix["true_reject"] / definite_negatives
                                if definite_negatives else None),
            },
            "numerical_minima": {
                "required_true_admit_minimum": 5,
                "required_true_reject_minimum": 5,
                "observed_true_admit": matrix["true_admit"],
                "observed_true_reject": matrix["true_reject"],
                "true_admit_minimum_met": matrix["true_admit"] >= 5,
                "true_reject_minimum_met": matrix["true_reject"] >= 5,
            },
            "integrity_pass": allowed,
            "allowlist_decision": {"allowlist_exact_classifier": allowed},
        }
        comparison_path = self.write_json(
            f"{temporal_root.name}/comparison.json", comparison_document)
        entry = {
            "classifier_spec_sha256": spec_sha,
            "spec": self.reference(spec, self.root),
            "validation": self.reference(comparison_path, self.root),
            "source_artifacts": relative_refs,
        }
        self.policy = {"qualified_temporal_classifiers": {
            classifier: {"classifier_spec_sha256": spec_sha}}}
        return {classifier: entry}, {
            "classifier": classifier,
            "comparison": comparison_path,
            "source_paths": source_paths,
            "clips": [temporal_root / "sources" / item["clip"] for item in manifest_items],
        }

    def write_bundle(self, document=None, *, secondary=None, controls=None, temporal=None):
        document = self.field_validation() if document is None else document
        secondary = self.visible_secondary_validation() if secondary is None else secondary
        controls = self.fault_controls() if controls is None else controls
        temporal = {} if temporal is None else temporal
        field_path = self.write_json("field.json", document)
        secondary_path = self.write_json("secondary.json", secondary)
        controls_path = self.write_json("controls.json", controls)
        manifest = {
            "schema_version": 1,
            "kind": "encounter_reader_qualification",
            "qualification_id": "test-reader-v1",
            "reader": {
                "method_version": 5,
                "implementation_sha256": copy.deepcopy(self.method),
                "runtime": copy.deepcopy(READER),
            },
            "camera": copy.deepcopy(CAMERA),
            "field_validation": self.reference(field_path, self.root),
            "visible_secondary_validation": self.reference(secondary_path, self.root),
            "fault_controls": self.reference(controls_path, self.root),
            "temporal_classifiers": temporal,
        }
        return self.write_json("qualification.json", manifest)

    def verify(self, path):
        def regenerated(image_path, _registration):
            return copy.deepcopy(self.reader_observations[digest(image_path)])

        def retained_rows(_classifier_id, _paths, _sources, _window, _selection, _items):
            return {index: {
                "source_frame_seq": 1000 + index,
                "capture_ns": 1_000_000_000 + index * 5_000_000,
            } for index in range(1000)}

        with (patch.object(encounter_qualification, "_observe_image", side_effect=regenerated),
              patch.object(encounter_qualification, "_validate_temporal_v2_media",
                           side_effect=retained_rows)):
            return verify_qualification(
                path,
                implementation_sha256=self.method,
                reader_runtime=READER,
                camera_name=CAMERA["name"],
                camera_profile=CAMERA["profile"],
                policy=self.policy,
                bench_source_sha256=BENCH_SHA,
            )

    def rewrite_temporal_comparison(self, temporal, context, mutate):
        document = json.loads(context["comparison"].read_text(encoding="utf-8"))
        mutate(document)
        context["comparison"].write_text(json.dumps(document, sort_keys=True), encoding="utf-8")
        classifier = context["classifier"]
        temporal[classifier]["validation"]["sha256"] = digest(context["comparison"])

    def test_complete_exact_bundle_qualifies(self):
        self.assertEqual(self.verify(self.write_bundle())["status"], "QUALIFIED")

    def test_complete_temporal_bundle_qualifies(self):
        temporal, _ = self.temporal_validation()
        result = self.verify(self.write_bundle(temporal=temporal))
        self.assertEqual(result["status"], "QUALIFIED")
        self.assertEqual(result["temporal_classifiers"]["v1-arrow-phase-edge-v2"]["total"], 10)

    def test_each_generic_temporal_v2_bundle_qualifies(self):
        classifiers = (
            "v1-main-bar-adjacent-redraw-v1",
            "v1-muted-badge-rising-fill-v1",
            "v1-unmute-stable-frequency-sweep-v1",
        )
        for classifier in classifiers:
            with self.subTest(classifier=classifier):
                temporal, _ = self.generic_temporal_validation(classifier)
                result = self.verify(self.write_bundle(temporal=temporal))
                self.assertEqual(result["status"], "QUALIFIED", result["errors"])
                self.assertEqual(result["temporal_classifiers"][classifier]["total"], 10)

    def test_generic_classifiers_cannot_use_arrow_schema_v1(self):
        for classifier in TEMPORAL_V2_OBSERVER_RUBRICS:
            with self.subTest(classifier=classifier):
                temporal, _ = self.generic_temporal_validation(
                    classifier, schema_version=1)
                result = self.verify(self.write_bundle(temporal=temporal))
                self.assertEqual(result["status"], "REJECTED")
                self.assertIn("schema version 1 is unsupported", result["errors"][0])

    def test_generic_temporal_binds_every_dependency_in_each_freeze(self):
        classifier = "v1-main-bar-adjacent-redraw-v1"
        for artifact in ("seal", "pre_pixel_freeze", "frozen_classifier_result"):
            for name in CLASSIFIER_IMPLEMENTATION_FILES[classifier]:
                with self.subTest(artifact=artifact, implementation=name):
                    temporal, _ = self.generic_temporal_validation(
                        classifier, binding_tamper=artifact, binding_file=name)
                    result = self.verify(self.write_bundle(temporal=temporal))
                    self.assertEqual(result["status"], "REJECTED")
                    self.assertTrue(result["errors"])

    def test_generic_temporal_binds_reader_in_each_freeze(self):
        classifier = "v1-muted-badge-rising-fill-v1"
        for artifact in ("seal", "pre_pixel_freeze", "frozen_classifier_result"):
            with self.subTest(artifact=artifact):
                temporal, _ = self.generic_temporal_validation(
                    classifier, reader_tamper=artifact)
                result = self.verify(self.write_bundle(temporal=temporal))
                self.assertEqual(result["status"], "REJECTED")
                self.assertTrue(result["errors"])

    def test_generic_temporal_authenticates_blind_capture_boundary(self):
        classifier = "v1-main-bar-adjacent-redraw-v1"
        for tamper in ("capture_bench", "capture_source", "capture_window", "capture_pixels"):
            with self.subTest(tamper=tamper):
                temporal, _ = self.generic_temporal_validation(
                    classifier, reader_tamper=tamper)
                result = self.verify(self.write_bundle(temporal=temporal))
                self.assertEqual(result["status"], "REJECTED")
                self.assertIn("qualification capture", result["errors"][0])

    def test_generic_temporal_selection_is_complete_and_well_formed(self):
        classifier = "v1-main-bar-adjacent-redraw-v1"
        for tamper in ("reordered", "wrong_type"):
            with self.subTest(tamper=tamper):
                temporal, _ = self.generic_temporal_validation(
                    classifier, selection_tamper=tamper)
                result = self.verify(self.write_bundle(temporal=temporal))
                self.assertEqual(result["status"], "REJECTED")
                self.assertTrue(any(word in result["errors"][0]
                                    for word in ("selection differs", "source document")),
                                result["errors"])

    def test_generic_temporal_cannot_duplicate_one_candidate_to_meet_minima(self):
        classifier = "v1-main-bar-adjacent-redraw-v1"
        temporal, _ = self.generic_temporal_validation(
            classifier, duplicate_candidate=True)
        result = self.verify(self.write_bundle(temporal=temporal))
        self.assertEqual(result["status"], "REJECTED")
        self.assertIn("source candidate is duplicated", result["errors"][0])

    def test_generic_temporal_spec_cannot_contradict_supported_minima_or_rubric(self):
        classifier = "v1-main-bar-adjacent-redraw-v1"
        temporal, _ = self.generic_temporal_validation(
            classifier, contradictory_spec=True)
        result = self.verify(self.write_bundle(temporal=temporal))
        self.assertEqual(result["status"], "REJECTED")
        self.assertIn("specification validation contract differs", result["errors"][0])

    def test_generic_temporal_blind_protocol_and_malformed_record_reject_cleanly(self):
        classifier = "v1-main-bar-adjacent-redraw-v1"
        for options in ({"blind_protocol_tamper": True},
                        {"malformed_admitted_record": True}):
            with self.subTest(options=options):
                temporal, _ = self.generic_temporal_validation(classifier, **options)
                result = self.verify(self.write_bundle(temporal=temporal))
                self.assertEqual(result["status"], "REJECTED")
                self.assertTrue(result["errors"])

    def test_generic_temporal_validates_complete_classifier_record_invariants(self):
        cases = (
            ("v1-main-bar-adjacent-redraw-v1", "bar_changed_index"),
            ("v1-main-bar-adjacent-redraw-v1", "bar_expectation"),
            ("v1-main-bar-adjacent-redraw-v1", "reader_binding"),
            ("v1-main-bar-adjacent-redraw-v1", "selection_binding"),
            ("v1-main-bar-adjacent-redraw-v1", "extra_field"),
            ("v1-main-bar-adjacent-redraw-v1", "analysis_result"),
            ("v1-muted-badge-rising-fill-v1", "badge_direction"),
            ("v1-muted-badge-rising-fill-v1", "stable_fields"),
            ("v1-muted-badge-rising-fill-v1", "support_sequence"),
            ("v1-unmute-stable-frequency-sweep-v1", "frequency_masks"),
            ("v1-unmute-stable-frequency-sweep-v1", "support_timestamp"),
            ("v1-unmute-stable-frequency-sweep-v1", "rejected_field"),
        )
        for classifier, tamper in cases:
            with self.subTest(classifier=classifier, tamper=tamper):
                temporal, _ = self.generic_temporal_validation(
                    classifier, record_tamper=tamper)
                result = self.verify(self.write_bundle(temporal=temporal))
                self.assertEqual(result["status"], "REJECTED")
                self.assertTrue(result["errors"])

    def test_generic_temporal_derives_clip_path_identity_and_exact_instructions(self):
        classifier = "v1-main-bar-adjacent-redraw-v1"
        for options in ({"clip_path_swap": True}, {"empty_readme": True}):
            with self.subTest(options=options):
                temporal, _ = self.generic_temporal_validation(classifier, **options)
                result = self.verify(self.write_bundle(temporal=temporal))
                self.assertEqual(result["status"], "REJECTED")
                self.assertTrue(result["errors"])

    def test_generic_temporal_observer_instructions_are_self_contained(self):
        for classifier, rubric in TEMPORAL_V2_OBSERVER_RUBRICS.items():
            with self.subTest(classifier=classifier):
                instructions = temporal_v2_observer_instructions(classifier)
                self.assertIn("clip_source_video_indices entry", instructions)
                self.assertIn("target_run_clip_frame_indices", instructions)
                self.assertIn("full_run_clip_frame_indices", instructions)
                self.assertIn("Never treat a full-run frame", instructions)
                self.assertIn("exact zero-based clip frames in the complete optical transition",
                              instructions)
                self.assertIn("Do not make that attestation if it is not true", instructions)
                self.assertIn("Allowed:", instructions)
                self.assertIn("Eligibility:", instructions)
                for field in rubric["literal_fields"]:
                    self.assertIn(f"- {field}:", instructions)
                for allowed in rubric["allowed_literals"].values():
                    for literal in allowed:
                        self.assertIn(literal, instructions)
        badge = temporal_v2_observer_instructions("v1-muted-badge-rising-fill-v1")
        self.assertIn("Uniform palette or brightness recoloring caused by mute is allowed", badge)
        self.assertIn("frequency, band, direction, bar geometry or count", badge)

    def test_generic_temporal_binds_clip_frame_mapping(self):
        classifier = "v1-main-bar-adjacent-redraw-v1"
        for tamper in ("source_gap", "target_position"):
            with self.subTest(tamper=tamper):
                temporal, _ = self.generic_temporal_validation(
                    classifier, clip_mapping_tamper=tamper)
                result = self.verify(self.write_bundle(temporal=temporal))
                self.assertEqual(result["status"], "REJECTED")
                self.assertIn("mapping", result["errors"][0])

    def test_generic_mute_rejection_contract_includes_event_scope(self):
        for classifier in ("v1-muted-badge-rising-fill-v1",
                           "v1-unmute-stable-frequency-sweep-v1"):
            with self.subTest(classifier=classifier):
                temporal, _ = self.generic_temporal_validation(
                    classifier, rejection_code="EVENT_SCOPE")
                result = self.verify(self.write_bundle(temporal=temporal))
                self.assertEqual(result["status"], "QUALIFIED", result["errors"])

    def test_generic_temporal_binds_code_owned_rubric(self):
        classifier = "v1-unmute-stable-frequency-sweep-v1"
        artifacts = ("seal", "pre_pixel_freeze", "frozen_classifier_result",
                     "observer_manifest", "completed_observations")
        for artifact in artifacts:
            with self.subTest(artifact=artifact):
                temporal, _ = self.generic_temporal_validation(
                    classifier, rubric_tamper=artifact)
                result = self.verify(self.write_bundle(temporal=temporal))
                self.assertEqual(result["status"], "REJECTED")
                self.assertTrue(any(word in result["errors"][0]
                                    for word in ("invalid", "differ")), result["errors"])

    def test_generic_temporal_comparison_tampering_is_rejected(self):
        mutations = {
            "stored eligibility": lambda document: document["comparisons"][0].update(
                observer_strict_visual_eligible_for_admission=False),
            "ground truth": lambda document: document["comparisons"][0].update(
                observer_ground_truth="DEFINITE_NEGATIVE"),
            "record hash": lambda document: document["comparisons"][0].update(
                frozen_classifier_record_sha256="b" * 64),
            "exact target list": lambda document: document["comparisons"][0].update(
                source_video_frame_indices=[0, 2]),
            "outcome": lambda document: document["comparisons"][0].update(
                comparison_outcome="FALSE_ADMIT"),
        }
        classifier = "v1-main-bar-adjacent-redraw-v1"
        for name, mutation in mutations.items():
            with self.subTest(tamper=name):
                temporal, context = self.generic_temporal_validation(classifier)
                self.rewrite_temporal_comparison(temporal, context, mutation)
                result = self.verify(self.write_bundle(temporal=temporal))
                self.assertEqual(result["status"], "REJECTED")
                self.assertIn("independently derived", result["errors"][0])

    def test_generic_temporal_false_admission_is_rejected_with_minima_met(self):
        cases = (
            ("v1-main-bar-adjacent-redraw-v1", {"false_admit": True}),
            ("v1-unmute-stable-frequency-sweep-v1", {"claim_mismatch": True}),
        )
        for classifier, options in cases:
            with self.subTest(classifier=classifier):
                temporal, _ = self.generic_temporal_validation(classifier, **options)
                result = self.verify(self.write_bundle(temporal=temporal))
                self.assertEqual(result["status"], "REJECTED")
                self.assertIn("integrity failed", result["errors"][0])

    def test_generic_temporal_indeterminate_observations_cannot_pay_negative_minimum(self):
        for classifier in TEMPORAL_V2_OBSERVER_RUBRICS:
            with self.subTest(classifier=classifier, decision="REJECTED"):
                temporal, context = self.generic_temporal_validation(
                    classifier, indeterminate_rejects=True)
                comparison = json.loads(context["comparison"].read_text(encoding="utf-8"))
                self.assertEqual(comparison["confusion_matrix"]["true_reject"], 0)
                self.assertEqual(comparison["confusion_matrix"]["abstain"], 5)
                self.assertEqual(comparison["denominators"]["observer_indeterminate"], 5)
                self.assertFalse(comparison["numerical_minima"]["true_reject_minimum_met"])
                self.assertTrue(all(
                    item["observer_ground_truth"] == "INDETERMINATE"
                    and item["comparison_outcome"] == "ABSTAIN"
                    for item in comparison["comparisons"][5:]))
                result = self.verify(self.write_bundle(temporal=temporal))
                self.assertEqual(result["status"], "REJECTED")
                self.assertIn("integrity failed", result["errors"][0])

            with self.subTest(classifier=classifier, decision="ADMITTED"):
                temporal, context = self.generic_temporal_validation(
                    classifier, false_admit=True, indeterminate_rejects=True)
                comparison = json.loads(context["comparison"].read_text(encoding="utf-8"))
                self.assertEqual(comparison["confusion_matrix"]["false_admit"], 1)
                self.assertEqual(
                    comparison["comparisons"][-1]["observer_ground_truth"],
                    "INDETERMINATE")
                self.assertEqual(
                    comparison["comparisons"][-1]["comparison_outcome"], "FALSE_ADMIT")
                result = self.verify(self.write_bundle(temporal=temporal))
                self.assertEqual(result["status"], "REJECTED")

    def test_generic_temporal_malformed_reference_and_nonfinite_json_reject_cleanly(self):
        classifier = "v1-main-bar-adjacent-redraw-v1"
        temporal, _ = self.generic_temporal_validation(classifier)
        temporal[classifier]["source_artifacts"]["selection"] = "not-an-evidence-reference"
        result = self.verify(self.write_bundle(temporal=temporal))
        self.assertEqual(result["status"], "REJECTED")
        self.assertIn("reference is malformed", result["errors"][0])

        temporal, context = self.generic_temporal_validation(classifier)
        document = json.loads(context["comparison"].read_text(encoding="utf-8"))
        document["rates"]["accuracy"] = float("nan")
        context["comparison"].write_text(json.dumps(document, sort_keys=True), encoding="utf-8")
        temporal[classifier]["validation"]["sha256"] = digest(context["comparison"])
        result = self.verify(self.write_bundle(temporal=temporal))
        self.assertEqual(result["status"], "REJECTED")
        self.assertIn("non-finite", result["errors"][0])

    def test_repository_policy_uses_only_supported_classifier_versions(self):
        policy_path = Path(__file__).resolve().parent / "bench" / "visible_event_policies.json"
        policies = json.loads(policy_path.read_text(encoding="utf-8"))["policies"]
        for policy in policies.values():
            identifiers = policy["qualified_temporal_classifier_ids"]
            self.assertEqual(set(identifiers), set(policy["qualified_temporal_classifiers"]))
            self.assertLessEqual(set(identifiers), set(CLASSIFIER_IMPLEMENTATION_FILES))
        self.assertNotIn("v1-arrow-phase-edge-v1", CLASSIFIER_IMPLEMENTATION_FILES)

    def test_missing_bundle_is_rejected(self):
        result = self.verify(None)
        self.assertEqual(result["status"], "REJECTED")
        self.assertIn("not supplied", result["errors"][0])

    def test_implementation_drift_is_rejected(self):
        path = self.write_bundle()
        self.method["encounter_reader.py"] = "c" * 64
        self.assertIn("running implementation differs", self.verify(path)["errors"][0])

    def test_camera_profile_drift_is_rejected(self):
        path = self.write_bundle()
        result = verify_qualification(
            path,
            implementation_sha256=self.method,
            reader_runtime=READER,
            camera_name=CAMERA["name"],
            camera_profile={"framerate": 60},
            policy=self.policy,
            bench_source_sha256=BENCH_SHA,
        )
        self.assertIn("camera profile differs", result["errors"][0])

    def test_one_wrong_blind_assertion_is_rejected(self):
        document = self.field_validation(wrong=True)
        result = self.verify(self.write_bundle(document))
        self.assertIn("wrong reader assertion", result["errors"][0])

    def test_one_missing_fault_control_is_rejected(self):
        controls = self.fault_controls(broken_control="missing_secondary")
        result = self.verify(self.write_bundle(controls=controls))
        self.assertIn("missing_secondary was not demonstrated", result["errors"][0])

    def test_stored_fault_comparison_tamper_is_rejected(self):
        controls = self.fault_controls()
        case = next(case for case in controls["cases"] if case["name"] == "wrong_strength")
        case["comparison"]["status"] = "MATCH"
        result = self.verify(self.write_bundle(controls=controls))
        self.assertIn("differs from independent comparison", result["errors"][0])

    def test_missing_required_unresolved_field_is_rejected(self):
        controls = self.fault_controls()
        case = next(case for case in controls["cases"] if case["name"] == "failure_with_unknown")
        case["required_unresolved"] = []
        result = self.verify(self.write_bundle(controls=controls))
        self.assertIn("wrong required unresolved fields", result["errors"][0])

    def test_empty_secondary_agreement_does_not_validate_visible_cards(self):
        secondary = self.visible_secondary_validation(empty=True)
        result = self.verify(self.write_bundle(secondary=secondary))
        self.assertIn("too few blind agreements on visible secondary cards", result["errors"][0])

    def test_wrong_visible_secondary_assertion_is_rejected(self):
        secondary = self.visible_secondary_validation(wrong=True)
        result = self.verify(self.write_bundle(secondary=secondary))
        self.assertIn("wrong reader assertion", result["errors"][0])

    def test_visible_secondary_source_hash_tamper_is_rejected(self):
        secondary = self.visible_secondary_validation()
        path = self.write_bundle(secondary=secondary)
        source = self.root / secondary["source_artifacts"]["blind_labels"]["path"]
        source.write_text("{}", encoding="utf-8")
        self.assertIn("evidence hash mismatch", self.verify(path)["errors"][0])

    def test_temporal_matrix_list_outcome_and_decision_tampering_is_rejected(self):
        mutations = {
            "matrix": lambda document: document["confusion_matrix"].update(true_admit=4),
            "list": lambda document: document["true_admit_ids"].pop(),
            "outcome": lambda document: document["comparisons"][0].update(
                comparison_outcome="FALSE_ADMIT"),
            "decision": lambda document: document["comparisons"][0].update(
                frozen_classifier_decision="REJECTED"),
        }
        expected = {
            "matrix": "matrix differs",
            "list": "list is inconsistent",
            "outcome": "comparison was not independently derived",
            "decision": "comparison was not independently derived",
        }
        for name, mutation in mutations.items():
            with self.subTest(tamper=name):
                temporal, context = self.temporal_validation()
                self.rewrite_temporal_comparison(temporal, context, mutation)
                result = self.verify(self.write_bundle(temporal=temporal))
                self.assertEqual(result["status"], "REJECTED")
                self.assertIn(expected[name], result["errors"][0])

    def test_temporal_source_artifact_tamper_is_rejected(self):
        temporal, context = self.temporal_validation()
        path = self.write_bundle(temporal=temporal)
        context["source_paths"]["completed_observations"].write_text("{}", encoding="utf-8")
        result = self.verify(path)
        self.assertIn("evidence hash mismatch", result["errors"][0])

    def test_temporal_clip_tamper_is_rejected(self):
        temporal, context = self.temporal_validation()
        path = self.write_bundle(temporal=temporal)
        context["clips"][0].write_bytes(b"tampered temporal clip")
        result = self.verify(path)
        self.assertIn("evidence hash mismatch", result["errors"][0])

    def test_evidence_path_cannot_escape_bundle(self):
        path = self.write_bundle()
        manifest = json.loads(path.read_text(encoding="utf-8"))
        manifest["field_validation"]["path"] = "../field.json"
        path.write_text(json.dumps(manifest), encoding="utf-8")
        self.assertIn("escapes its bundle", self.verify(path)["errors"][0])


if __name__ == "__main__":
    unittest.main()
