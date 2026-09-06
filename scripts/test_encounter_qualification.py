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
from encounter_runtime_probe import probe_image_sha256
import encounter_qualification
from encounter_qualification import (
    CLASSIFIER_IMPLEMENTATION_FILES,
    COMMON_TEMPORAL_IMPLEMENTATION_FILES,
    CORE_READER_FILES,
    FIELDS,
    REQUIRED_FAULT_CONTROLS,
    STATIC_READER_IMPLEMENTATION_FILES,
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
    "ocr_runtime_probe": {
        "status": "operational",
        "probe_sha256": probe_image_sha256(),
    },
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
        names = (*STATIC_READER_IMPLEMENTATION_FILES,
                 *COMMON_TEMPORAL_IMPLEMENTATION_FILES,
                 "encounter_arrow_transition.py",
                 "encounter_arrow_acquisition.py",
                 "encounter_bar_transition.py", "encounter_mute_redraw_transition.py",
                 "encounter_redraw_probe.py", "encounter_frequency_context.py",
                 "encounter_secondary_context.py",
                 "encounter_secondary_optical_bridge.py", "encounter_secondary_probe.py")
        self.method = {name: SHA for name in names}
        self.policy = {"contract_version": 3, "qualified_temporal_classifiers": {}}
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

    def field_validation(self, *, wrong=False, empty_control_count=6,
                         ghost_partial_identity=False):
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
                if field == "secondary" and index < empty_control_count:
                    reference = {"state": "readable", "value": []}
                blind[field] = copy.deepcopy(reference)
                observed = copy.deepcopy(reference)
                status = "AGREEMENT"
                if wrong and index == 0 and field == "counter_glyph":
                    observed = {"state": "readable", "value": "8"}
                    status = "WRONG_ASSERTION"
                if ghost_partial_identity and index == 0 and field == "secondary":
                    observed = {
                        "state": "unreadable", "value": None,
                        "reason": "partial card",
                        "partial_cards": [{"band": "Ka", "frequency": "34.700",
                                           "direction": None, "bars": None}],
                    }
                    status = "READER_REFUSAL"
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

    def visible_secondary_validation(self, *, empty=False, wrong=False,
                                     wrong_partial=False, partial_identity_count=6,
                                     unresolved_partial=False,
                                     unresolved_full_assertion=False,
                                     reanalysis=False):
        packet_id = "secondary-packet-test-v1"
        image_integrity = {}
        single_labels = []
        single_hidden = []
        items = []
        counts = Counter()
        for index in range(12):
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
            if unresolved_partial and index == 6:
                cards[0] = {
                    "band": "uncertain", "frequency": "uncertain",
                    "direction": "uncertain", "bar_count": None,
                    "meter_cells": ["uncertain"] * 6,
                }
            if unresolved_full_assertion and index == 5:
                cards[0]["direction"] = "uncertain"
            blind_label = {
                "image": source_image,
                "display_visibility": "visible",
                "card_count": len(cards),
                "cards": cards,
            }
            reference = encounter_qualification._blind_secondary_reference(blind_label)
            observed = copy.deepcopy(reference)
            if unresolved_full_assertion and index == 5:
                observed = {
                    "state": "readable",
                    "value": [{"band": "Ka", "frequency": "34.700",
                               "direction": "front", "bars": 3}],
                }
            if wrong and index == 0:
                observed["value"][0]["bars"] = 4
            if not empty and index >= 6:
                identity_index = index - 6
                observed = {
                    "state": "unreadable", "value": None,
                    "reason": "direction and bars are unresolved",
                    "partial_cards": [{
                        "band": "Ka" if identity_index < partial_identity_count else None,
                        "frequency": "34.700" if identity_index < partial_identity_count else None,
                        "direction": None, "bars": None,
                    }],
                }
                if wrong_partial and index == 6:
                    observed["partial_cards"][0].update(band="K", frequency="24.150")
            status = encounter_qualification._derived_field_status(
                "secondary", observed, reference)
            counts[status] += 1
            self.reader_observations[image_sha] = {"secondary": copy.deepcopy(observed)}
            single_labels.append({"item_id": f"single-{index:02d}", "frame": blind_label})
            source_observed = (copy.deepcopy(reference) if reanalysis
                               else copy.deepcopy(observed))
            single_hidden.append({"item_id": f"single-{index:02d}",
                                  "image": source_image,
                                  "machine_secondary": source_observed})
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
            "implementation": {
                "reader_sha256": "d" * 64 if reanalysis
                else self.method["encounter_reader.py"],
                "reader_setup": ({"method_version": 4} if reanalysis
                                 else copy.deepcopy(READER)),
            },
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
        document = {
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
        if reanalysis:
            document["reader_reanalysis"] = {
                "kind": "complete_exact_reader_reread",
                "source_sealed_key_sha256": sources["sealed_key"]["sha256"],
                "source_method_version": 4,
                "source_reader_sha256": "d" * 64,
                "current_method_version": READER["method_version"],
                "current_reader_sha256": self.method["encounter_reader.py"],
                "complete_source_set_reread": True,
            }
        return document

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
            "implementation_sha256": {
                name: self.method[name]
                for name in CLASSIFIER_IMPLEMENTATION_FILES[classifier]
            },
            "spec": self.reference(spec, self.root),
            "validation": self.reference(comparison_path, self.root),
            "source_artifacts": relative_refs,
        }
        self.policy = {"contract_version": 1, "qualified_temporal_classifiers": {
            classifier: {
                "classifier_spec_sha256": spec_sha,
                "raw_affected_fields": ["main_arrows"],
            }}}
        return {classifier: entry}, {
            "classifier": classifier,
            "comparison": comparison_path,
            "source_paths": source_paths,
            "clips": [temporal_root / "sources" / item["clip"] for item in manifest_items],
        }

    @staticmethod
    def generic_temporal_literal(classifier, eligible, *, indeterminate=False):
        if classifier == "v1-stable-frequency-intact-context-v1":
            return ({
                "frequency_glyph_relation": "SAME_FREQUENCY_GLYPHS_THROUGHOUT",
                "segment_integrity": "ALL_SEGMENTS_COMPLETE",
                "endpoint_support": "BOTH_CLEAR",
                "target_content": "LEGAL_TARGET_CONTENT",
                "confidence": "HIGH",
            } if eligible else ({
                "frequency_glyph_relation": "VISUALLY_INDETERMINATE",
                "segment_integrity": "INDETERMINATE",
                "endpoint_support": "INDETERMINATE",
                "target_content": "INDETERMINATE",
                "confidence": "LOW",
            } if indeterminate else {
                "frequency_glyph_relation": "FREQUENCY_GLYPHS_CHANGE",
                "segment_integrity": "ALL_SEGMENTS_COMPLETE",
                "endpoint_support": "BOTH_CLEAR",
                "target_content": "NOT_LEGAL_TARGET_CONTENT",
                "confidence": "HIGH",
            }))
        if classifier == "v1-arrow-target-acquisition-v1":
            return ({
                "endpoint_support": "BOTH_CLEAR",
                "endpoint_relation": "PRIOR_OR_PRIOR_PLUS_CURRENT_TO_CURRENT",
                "transition_class": "COHERENT_CHANGED_DIRECTION_MOTION",
                "claimed_frame_acquisition": (
                    "EVERY_CLAIMED_FRAME_HAS_NONCURRENT_CHANGED_DIRECTION"),
                "unchanged_direction_motion": "NO",
                "confidence": "HIGH",
            } if eligible else ({
                "endpoint_support": "INDETERMINATE",
                "endpoint_relation": "INDETERMINATE",
                "transition_class": "VISUALLY_INDETERMINATE",
                "claimed_frame_acquisition": "VISUALLY_INDETERMINATE",
                "unchanged_direction_motion": "INDETERMINATE",
                "confidence": "LOW",
            } if indeterminate else {
                "endpoint_support": "BOTH_CLEAR",
                "endpoint_relation": "OTHER_ENDPOINT_RELATION",
                "transition_class": "NONCOHERENT_CHANGED_DIRECTION_MOTION",
                "claimed_frame_acquisition": (
                    "A_CLAIMED_FRAME_IS_CURRENT_IN_ALL_CHANGED_DIRECTIONS"),
                "unchanged_direction_motion": "YES",
                "confidence": "HIGH",
            }))
        if classifier == "v1-secondary-closed-context-v3":
            return ({
                "card_context": "SAME_CURRENT_CARD_CONTEXT",
                "support_pairs": "BOTH_CLEAR",
                "meter_redraw": "COHERENT_PARTIAL_METER_REDRAW",
                "count_closure": "EXACT_COUNT_CLOSURE",
                "confidence": "HIGH",
            } if eligible else ({
                "card_context": "VISUALLY_INDETERMINATE",
                "support_pairs": "INDETERMINATE",
                "meter_redraw": "VISUALLY_INDETERMINATE",
                "count_closure": "INDETERMINATE",
                "confidence": "LOW",
            } if indeterminate else {
                "card_context": "CARD_CONTEXT_CHANGES",
                "support_pairs": "BOTH_CLEAR",
                "meter_redraw": "INCOMPATIBLE_METER_CONTENT",
                "count_closure": "COUNT_NOT_UNIQUELY_CLOSED",
                "confidence": "HIGH",
            }))
        if classifier == "v1-secondary-text-optical-bridge-v1":
            return ({
                "support_pairs": "BOTH_CLEAR",
                "center_text": "CENTER_COMPLETE_TEXT_CLEAR",
                "card_text_relation": "SAME_COMPLETE_CARD_TEXT_THROUGHOUT",
                "direction_or_meter_change": "NO_DIRECTION_OR_METER_CHANGE",
                "confidence": "HIGH",
            } if eligible else ({
                "support_pairs": "INDETERMINATE",
                "center_text": "VISUALLY_INDETERMINATE",
                "card_text_relation": "VISUALLY_INDETERMINATE",
                "direction_or_meter_change": "VISUALLY_INDETERMINATE",
                "confidence": "LOW",
            } if indeterminate else {
                "support_pairs": "BOTH_CLEAR",
                "center_text": "CENTER_COMPLETE_TEXT_NOT_CLEAR",
                "card_text_relation": "CARD_TEXT_CHANGES",
                "direction_or_meter_change": "DIRECTION_OR_METER_CHANGE",
                "confidence": "HIGH",
            }))
        if classifier == "v1-arrow-phase-edge-v5":
            return ({
                "center_class": "COHERENT_SINGLE_DIRECTION_ON_OFF_EDGE",
                "endpoint_support": "BOTH_CLEAR",
                "extra_direction_motion": "NO",
                "confidence": "HIGH",
            } if eligible else ({
                "center_class": "VISUALLY_INDETERMINATE",
                "endpoint_support": "INDETERMINATE",
                "extra_direction_motion": "INDETERMINATE",
                "confidence": "LOW",
            } if indeterminate else {
                "center_class": "NOT_COHERENT_SINGLE_DIRECTION_ON_OFF_EDGE",
                "endpoint_support": "BOTH_CLEAR",
                "extra_direction_motion": "YES",
                "confidence": "HIGH",
            }))
        if classifier == "v1-main-bar-adjacent-redraw-v2":
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
        if classifier == "v1-muted-badge-rising-fill-v2":
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
        if classifier == "v1-unmute-stable-frequency-sweep-v2":
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
                                    indeterminate_rejects=False,
                                    recording_maximum_interval_ns=5_000_000,
                                    source_gap_at_index=None,
                                    frequency_below_minimum=False,
                                    frequency_missing_band=False,
                                    optical_missing_band=False,
                                    observer_claim_tamper=False,
                                    extra_rejected_candidates=0):
        temporal_root = self.root / f"temporal-v2-{classifier}"
        repository_spec = (Path(__file__).resolve().parent / "bench" / "temporal_specs" /
                           f"{classifier}.json")
        spec_document = json.loads(repository_spec.read_text(encoding="utf-8"))
        spec_identity = {
            "reader_method_version": READER["method_version"],
            "reader_sha256": self.method["encounter_reader.py"],
        }
        if classifier == "v1-secondary-text-optical-bridge-v1":
            spec_identity.update(
                secondary_probe_method_version=1,
                secondary_probe_sha256=self.method["encounter_secondary_probe.py"])
        elif classifier not in {
                "v1-arrow-phase-edge-v5", "v1-arrow-target-acquisition-v1",
                "v1-secondary-closed-context-v3"}:
            spec_identity.update(
                redraw_probe_method_version=1,
                redraw_probe_sha256=self.method["encounter_redraw_probe.py"])
        spec_document["identity"] = spec_identity
        if contradictory_spec:
            if classifier == "v1-arrow-phase-edge-v5":
                spec_document["qualification_requirements"]["minimum_blind_true_admits"] = 50
            elif classifier == "v1-arrow-target-acquisition-v1":
                spec_document["deadline_observation_semantics"] = "LEGAL_PRESENTATION_TRANSITION"
            elif classifier == "v1-stable-frequency-intact-context-v1":
                spec_document["validation"]["branch_gates"]["intact_mask"][
                    "minimum_blind_true_admits"] = 50
            else:
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
        frequency_context = classifier == "v1-stable-frequency-intact-context-v1"
        secondary_optical = classifier == "v1-secondary-text-optical-bridge-v1"
        if secondary_optical and rejection_code == "UNCLOSED_RUN":
            rejection_code = "UNCLOSED_BRACKET"
        item_count = (11 if false_admit or claim_mismatch else 10) + extra_rejected_candidates
        branch_by_opaque = {}
        band_by_opaque = {}
        analysis_events = []
        affected = TEMPORAL_V2_OBSERVER_RUBRICS[classifier]["raw_affected_fields"]
        admitted_count = (4 if frequency_context and frequency_below_minimum else 5)
        admitted_count += int(bool(false_admit or claim_mismatch))
        selection_document = encounter_qualification.temporal_selection_document(
            admitted_count, item_count - admitted_count,
            [f"opaque-{index:02d}" for index in range(item_count)])
        if selection_tamper == "reordered":
            selection_document["opaque_ids"].reverse()
        selection_path = self.write_json(
            f"{temporal_root.name}/sources/selection.json",
            [] if selection_tamper == "wrong_type" else selection_document)
        capture_id = "d" * 64
        def source_time(index):
            shift = (10_000_000 if source_gap_at_index is not None
                     and index >= source_gap_at_index else 0)
            return 1_000_000_000 + index * 5_000_000 + shift
        support_padding = 3 if classifier == "v1-arrow-phase-edge-v5" else 2
        analysis_indices = sorted({value for index in range(item_count)
                                   for value in range(10 + index * 3 - support_padding,
                                                      13 + index * 3 + support_padding)})
        analysis_selection_path = self.write_json(
            f"{temporal_root.name}/sources/analysis-selection.json", {
                "schema_version": 1,
                "identity": {"capture_id": capture_id},
                "selection_mode": "qualification_temporal_candidates",
                "samples": [{"video_frame_index": value,
                             "source_frame_seq": 1000 + value,
                             "capture_ns": source_time(value)}
                            for value in analysis_indices],
            })
        record_context = {
            "capture_id": capture_id,
            "selection_manifest_sha256": digest(analysis_selection_path),
            "verified_maximum_source_interval_ns": recording_maximum_interval_ns,
            "reader_method_version": READER["method_version"],
            "reader_sha256": self.method["encounter_reader.py"],
        }
        if classifier == "v1-secondary-text-optical-bridge-v1":
            record_context.update(
                secondary_probe_method_version=1,
                secondary_probe_sha256=self.method["encounter_secondary_probe.py"])
        elif classifier not in {
                "v1-arrow-phase-edge-v5", "v1-arrow-target-acquisition-v1",
                "v1-secondary-closed-context-v3"}:
            record_context.update(
                redraw_probe_method_version=1,
                redraw_probe_sha256=self.method["encounter_redraw_probe.py"])

        def point(video_index):
            return {
                "video_frame_index": video_index,
                "source_frame_seq": 1000 + video_index,
                "capture_ns": source_time(video_index),
            }

        for index in range(item_count):
            opaque_id = f"opaque-{index:02d}"
            extra_index = 10
            claim_mismatch_item = claim_mismatch and index == extra_index
            visually_eligible = (index < (4 if frequency_context and frequency_below_minimum else 5)
                                 or claim_mismatch_item)
            machine_admitted = visually_eligible or (false_admit and index == extra_index)
            decision = "ADMITTED" if machine_admitted else "REJECTED"
            branch = "intact_mask"
            admitted_band_order = (["X", "K", "X", "K", "X"]
                                   if (frequency_missing_band or optical_missing_band) else
                                   ["X", "K", "Ka", "X", "K"])
            band = (admitted_band_order[index % 5]
                    if (frequency_context
                        or classifier == "v1-secondary-text-optical-bridge-v1")
                    and visually_eligible
                    else "Ka")
            frequency = {"X": "10.525", "K": "24.150", "Ka": "34.700"}[band]
            if frequency_context:
                branch_by_opaque[opaque_id] = branch
                band_by_opaque[opaque_id] = band
            elif secondary_optical:
                band_by_opaque[opaque_id] = band
            literal_observation = self.generic_temporal_literal(
                classifier, visually_eligible,
                indeterminate=indeterminate_rejects and not visually_eligible)
            observer_indeterminate = indeterminate_rejects and not visually_eligible
            if classifier == "v1-arrow-phase-edge-v5":
                literal_observation.update(
                    left_endpoint_directions=None if observer_indeterminate else [],
                    right_endpoint_directions=(None if observer_indeterminate else
                                               (["side"] if observer_claim_tamper and index == 0
                                                else ["front"])))
            elif classifier == "v1-arrow-target-acquisition-v1":
                literal_observation.update(
                    left_endpoint_directions=(None if observer_indeterminate
                                              else ["front", "side"]),
                    right_endpoint_directions=(None if observer_indeterminate else
                                               (["side"] if observer_claim_tamper and index == 0
                                                else ["front"])))
            elif classifier == "v1-stable-frequency-intact-context-v1":
                literal_observation["observed_frequency"] = (
                    None if observer_indeterminate else
                    "35.500" if observer_claim_tamper and index == 0 else frequency)
            elif classifier in {
                    "v1-secondary-closed-context-v3",
                    "v1-secondary-text-optical-bridge-v1"}:
                observed_card = {
                    "band": band, "frequency": frequency,
                    "direction": "front",
                    "bars": 4 if observer_claim_tamper and index == 0 else 3,
                }
                if classifier == "v1-secondary-closed-context-v3":
                    literal_observation["observed_cards"] = (
                        None if observer_indeterminate else [observed_card])
                else:
                    literal_observation["support_cards"] = (
                        None if observer_indeterminate else [copy.deepcopy(observed_card)])
                    literal_observation["center_cards"] = (
                        None if observer_indeterminate else [copy.deepcopy(observed_card)])
            _, ground_truth = encounter_qualification._temporal_v2_observer_ground_truth(
                classifier, {"opaque_id": opaque_id, **literal_observation})
            ground_truth_counts[ground_truth] += 1
            indices = ([11 + index * 3]
                       if classifier == "v1-secondary-text-optical-bridge-v1" else
                       [10 + index * 3, 11 + index * 3, 12 + index * 3])
            if duplicate_candidate and index == 1:
                indices = [10, 11, 12]
            event_id = "event-0000" if duplicate_candidate and index == 1 else f"event-{index:04d}"
            if frequency_context:
                analysis_events.append({
                    "event_id": event_id,
                    "mode": "UNCHANGED",
                    "changed_fields": [],
                    "target": {"fields": {
                        "primary_frequency": {"allowed": [frequency]}}},
                    "wire_rows": [{
                        "band": band.casefold(),
                        "frequency": frequency,
                        "priority": True,
                    }],
                })
            elif classifier in {
                    "v1-secondary-closed-context-v3",
                    "v1-secondary-text-optical-bridge-v1"}:
                support_card = {
                    "band": band, "frequency": frequency,
                    "direction": "front", "bars": 3}
                analysis_events.append({
                    "event_id": event_id,
                    "mode": "UNCHANGED",
                    "changed_fields": [],
                    "first_correct": point(indices[0] - 3),
                    "target": {"fields": {
                        "secondary": {"allowed": [[copy.deepcopy(support_card)]]}}},
                })
            full_indices = ([indices[0] - 2, indices[0] - 1, *indices,
                             indices[-1] + 1, indices[-1] + 2]
                            if classifier == "v1-secondary-closed-context-v3"
                            and machine_admitted else
                            list(range(indices[0] - 2, indices[0] + 3))
                            if classifier == "v1-secondary-text-optical-bridge-v1"
                            and machine_admitted else
                            [indices[0] - 1, *indices]
                            if classifier == "v1-unmute-stable-frequency-sweep-v2"
                            and machine_admitted else list(indices))
            clip = temporal_root / "sources" / "clips" / f"{opaque_id}.bin"
            clip.parent.mkdir(parents=True, exist_ok=True)
            clip.write_bytes(f"qualified generic temporal clip {opaque_id}".encode("ascii"))
            clip_sha = digest(clip)
            size = clip.stat().st_size
            fixture_source_rows = {
                value: {"source_frame_seq": 1000 + value,
                        "capture_ns": source_time(value)}
                for value in range(1000)
            }
            clip_source_indices = (
                encounter_qualification.temporal_observer_clip_source_indices(
                    indices, fixture_source_rows))
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
                if classifier == "v1-arrow-phase-edge-v5":
                    constants = spec_document["constants"]
                    record.update({
                        "left_support": point(indices[0] - 3),
                        "left_endpoint": point(indices[0] - 2),
                        "right_endpoint": point(indices[-1] + 2),
                        "right_support": point(indices[-1] + 3),
                        "endpoint_values": [[], ["front"]],
                        "changed_direction": "front",
                        "arrow_expectation_signature": {
                            "allowed_arrow_sets": [[], ["front"]],
                            "joint_arrow_phases": [[], ["front"]],
                        },
                        "endpoint_separation_rms": 60.0,
                        "profile_frame_indices": list(range(indices[0] - 2, indices[-1] + 3)),
                        "projections": [0.0, 0.1, 0.25, 0.5, 0.75, 0.95, 1.0],
                        "normalized_residuals": [0.0, 0.01, 0.01, 0.01, 0.01, 0.01, 0.0],
                        "maximum_backward_step": 0.0,
                        "total_backward_motion": 0.0,
                        "extra_direction_profile_diameter_rms": {
                            "side": 1.0, "rear": 1.0},
                        "maximum_endpoint_span_ns":
                            constants["authored_blink_phase_ns"] +
                            recording_maximum_interval_ns,
                        "profile_schema": {
                            "rows": 4, "columns": 4, "cells": 16,
                            "sample": "max-channel cell median"},
                        "profile_reference_bounds": {
                            "front": [1008, 203, 1145, 293],
                            "side": [1003, 313, 1153, 345],
                            "rear": [1044, 366, 1113, 392],
                        },
                    })
                elif classifier == "v1-arrow-target-acquisition-v1":
                    constants = spec_document["constants"]
                    changed = "side"
                    record.update({
                        "deadline_observation_semantics":
                            "TARGET_ACQUISITION_TRANSITION",
                        "full_transition_indices": full_indices,
                        "left_support": [point(indices[0] - 2), point(indices[0] - 1)],
                        "right_support": [point(indices[-1] + 1), point(indices[-1] + 2)],
                        "endpoint_values": [["front", "side"], ["front"]],
                        "endpoint_phase_basis": {
                            "previous_phase": ["side"],
                            "current_phase": ["front"],
                            "left_phase": ["front", "side"],
                            "right_phase": ["front"],
                        },
                        "changed_directions": [changed],
                        "arrow_expectation_signature": {
                            "previous_arrow_sets": [["side"]],
                            "current_arrow_sets": [["front"]],
                        },
                        "direction_metrics": {changed: {
                            "endpoint_separation_rms": 60.0,
                            "projections": [0.25, 0.5, 0.75],
                            "normalized_residuals": [0.01, 0.01, 0.01],
                            "maximum_backward_step": 0.0,
                            "total_backward_motion": 0.0,
                        }},
                        "claimed_frame_acquisition_proof": [{
                            "video_frame_index": value,
                            "changed_direction_states": {changed: "partial"},
                            "noncurrent_changed_directions": [changed],
                        } for value in indices],
                        "unchanged_direction_profile_diameter_rms": {
                            "front": 1.0, "rear": 1.0},
                        "maximum_endpoint_span_ns":
                            constants["authored_display_update_ns"] +
                            min(recording_maximum_interval_ns,
                                constants["maximum_support_interval_ns"]),
                        "support_search_frames_each_side":
                            constants["support_search_frames_each_side"],
                        "profile_schema": {
                            "rows": 4, "columns": 4, "cells": 16,
                            "sample": "max-channel cell median"},
                        "profile_reference_bounds": {
                            "front": [1008, 203, 1145, 293],
                            "side": [1003, 313, 1153, 345],
                            "rear": [1044, 366, 1113, 392],
                        },
                    })
                elif classifier == "v1-stable-frequency-intact-context-v1":
                    constants = spec_document["constants"]
                    masks = [encounter_qualification._DIGIT_MASKS[digit]
                             for digit in frequency.replace(".", "")]
                    record.update({
                        "deadline_observation_semantics": "LEGAL_PRESENTATION_TRANSITION",
                        "verification_closure_semantics":
                            "RAW_CURRENT_BRACKETED_UNRESOLVED_VERIFICATION_BOUNDARY",
                        "branch": branch,
                        "left_support": [point(indices[0] - 2), point(indices[0] - 1)],
                        "right_support": [point(indices[-1] + 1), point(indices[-1] + 2)],
                        "context_frame_indices": full_indices,
                        "context_observed_branches": [branch],
                        "support_derived_frequency": frequency,
                        "support_derived_digit_masks": masks,
                        "ambiguity_reason": spec_document["branches"][branch][
                            "ambiguity_reason"],
                        "maximum_refusal_run_span_ns":
                            constants["maximum_refusal_run_span_ns"],
                        "maximum_support_chain_span_ns":
                            constants["maximum_support_chain_span_ns"],
                        "event_signature": {
                            "mode": "UNCHANGED", "changed_fields": [],
                            "current_primary_frequency": frequency,
                        },
                    })
                elif classifier == "v1-secondary-text-optical-bridge-v1":
                    constants = spec_document["constants"]
                    profile = spec_document["profile"]
                    support = [{
                        "band": band, "frequency": frequency,
                        "direction": "front", "bars": 3}]
                    record.update({
                        "deadline_observation_semantics":
                            "LEGAL_PRESENTATION_TRANSITION",
                        "left_support": [point(indices[0] - 2), point(indices[0] - 1)],
                        "right_support": [point(indices[0] + 1), point(indices[0] + 2)],
                        "current_presentation_established": point(indices[0] - 3),
                        "deficient_slot": 0,
                        "raw_frequency_only_ocr": {
                            "text": frequency,
                            "normalized_frequency": frequency,
                            "confidence": 1.0,
                        },
                        "support_derived_secondary": support,
                        "resolved_value": copy.deepcopy(support),
                        "profile_schema": {
                            "rows": profile["rows"],
                            "columns": profile["columns"],
                            "channels": profile["channels"],
                            "order": profile["order"],
                            "sample": profile["sample"],
                            "normalization": profile["normalization"],
                        },
                        "profile_reference_bounds": profile["boxes"][0],
                        "profile_sha256s": [str(value) * 64 for value in "abcde"],
                        "profile_metrics": {
                            "maximum_support_pair_rms": 0.0,
                            "maximum_support_component_span": 0,
                            "maximum_target_support_rms": 0.0,
                            "maximum_target_envelope_excursion": 0,
                            "target_envelope_violation_count": 0,
                        },
                        "profile_limits": {
                            "maximum_support_pair_rms":
                                constants["maximum_support_pair_rms"],
                            "maximum_support_component_span":
                                constants["maximum_support_component_span"],
                            "maximum_target_support_rms":
                                constants["maximum_target_support_rms"],
                            "maximum_target_envelope_excursion":
                                constants["maximum_target_envelope_excursion"],
                        },
                        "maximum_support_chain_span_ns":
                            constants["maximum_support_chain_span_ns"],
                        "maximum_support_chain_interval_ns":
                            min(recording_maximum_interval_ns,
                                constants["maximum_support_chain_interval_ns"]),
                        "event_signature": {
                            "mode": "UNCHANGED",
                            "changed_fields": [],
                            "current_secondary": copy.deepcopy(support),
                        },
                    })
                elif classifier == "v1-secondary-closed-context-v3":
                    constants = spec_document["constants"]
                    support = [copy.deepcopy(CARD)]
                    meter_states = (
                        ["on", "on", "partial", "off", "off", "off"],
                        ["on", "on", "on", "partial", "off", "off"],
                        ["on", "on", "partial", "off", "off", "off"],
                    )
                    meter_evidence = []
                    for value, states in zip(indices, meter_states):
                        compatible = [count for count in range(7) if all(
                            state == "partial"
                            or (state == "on" and cell < count)
                            or (state == "off" and cell >= count)
                            for cell, state in enumerate(states))]
                        meter_evidence.append({
                            "video_frame_index": value,
                            "cards": [{
                                "slot": 0,
                                "state": "partial",
                                "bars": None,
                                "partial_cells": [states.index("partial")],
                                "compatible_bars": compatible,
                                "cell_states": list(states),
                            }],
                        })
                    record.update({
                        "deadline_observation_semantics":
                            "LEGAL_PRESENTATION_TRANSITION",
                        "verification_closure_semantics": spec_document["verification_closure_semantics"],
                        "auxiliary_closure_context_ns": spec_document["auxiliary_closure_context_ns"],
                        "context_frame_indices": full_indices,
                        "full_context_indices": full_indices,
                        "context_refusal_indices": indices,
                        "interleaved_readable_indices": [],
                        "context_first": point(indices[0]),
                        "context_last": point(indices[-1]),
                        "left_support": [point(full_indices[0]), point(full_indices[1])],
                        "right_support": [point(full_indices[-2]), point(full_indices[-1])],
                        "current_presentation_established": point(indices[0] - 3),
                        "support_derived_secondary": support,
                        "resolved_value": copy.deepcopy(support),
                        "partial_meter_evidence": meter_evidence,
                        "compatible_bar_intersections": [[CARD["bars"]]],
                        "maximum_interleaved_readable_frames":
                            constants["maximum_interleaved_readable_frames"],
                        "maximum_context_refusal_span_ns":
                            constants["authored_display_update_ns"] +
                            min(recording_maximum_interval_ns,
                                constants["maximum_support_chain_interval_ns"]),
                        "maximum_support_chain_span_ns":
                            constants["maximum_support_chain_span_ns"],
                        "maximum_support_chain_interval_ns":
                            min(recording_maximum_interval_ns,
                                constants["maximum_support_chain_interval_ns"]),
                        "event_signature": {
                            "mode": "UNCHANGED",
                            "changed_fields": [],
                            "current_secondary": copy.deepcopy(support),
                        },
                    })
                elif classifier == "v1-main-bar-adjacent-redraw-v2":
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
                            min(recording_maximum_interval_ns, 10_000_000),
                        "profile_schema": {
                            "rows": profile["rows"], "columns": profile["columns"],
                            "sample": profile["sample"]},
                        "profile_boxes": profile["bar_boxes_bottom_to_top"],
                    })
                elif classifier == "v1-muted-badge-rising-fill-v2":
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
                    if record_tamper == "arrow_left_guard":
                        record["left_support"] = point(indices[0] - 2)
                    elif record_tamper == "arrow_right_guard":
                        record["right_support"] = point(indices[-1] + 2)
                    elif record_tamper == "arrow_left_endpoint":
                        record["left_endpoint"] = point(indices[0] - 1)
                    elif record_tamper == "arrow_right_endpoint":
                        record["right_endpoint"] = point(indices[-1] + 1)
                    elif record_tamper == "arrow_guard_sequence":
                        record["left_support"]["source_frame_seq"] += 1
                    elif record_tamper == "arrow_guard_timestamp":
                        record["right_support"]["capture_ns"] += 1
                    elif record_tamper == "arrow_inner_metrics":
                        for name in ("profile_frame_indices", "projections", "normalized_residuals"):
                            del record[name][1]
                    elif record_tamper == "arrow_projection_bound":
                        record["projections"][-2] = 1.06
                    elif record_tamper == "arrow_residual_bound":
                        record["normalized_residuals"][1] = 0.151
                    elif record_tamper == "arrow_endpoint_span":
                        record["maximum_endpoint_span_ns"] += 1
                    elif record_tamper == "bar_changed_index":
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
                    elif record_tamper == "acquisition_phase":
                        record["endpoint_phase_basis"]["previous_phase"] = ["rear"]
                    elif record_tamper == "acquisition_motion":
                        record["direction_metrics"]["side"]["projections"] = [
                            0.25, 0.75, 0.5]
                    elif record_tamper == "acquisition_unchanged":
                        record["unchanged_direction_profile_diameter_rms"]["front"] = 9.0
                    elif record_tamper == "acquisition_claim_order":
                        record["claimed_frame_acquisition_proof"].reverse()
                    elif record_tamper == "acquisition_claim_state_keys":
                        record["claimed_frame_acquisition_proof"][0][
                            "changed_direction_states"]["rear"] = "unlit"
                    elif record_tamper == "acquisition_claim_noncurrent":
                        record["claimed_frame_acquisition_proof"][0][
                            "noncurrent_changed_directions"] = []
                    elif record_tamper == "frequency_context_branch":
                        record["branch"] = "invented_branch"
                    elif record_tamper == "frequency_context_masks":
                        record["support_derived_digit_masks"][0] = "abcdefg"
                    elif record_tamper == "frequency_context_partial":
                        record["context_observed_branches"].append("partial_expected_on_segments")
                    elif record_tamper == "frequency_context_closure":
                        record["verification_closure_semantics"] = "INVENTED_CLOSURE"
                    elif record_tamper == "secondary_context_established":
                        record["current_presentation_established"]["capture_ns"] = (
                            record["context_first"]["capture_ns"] + 1)
                    elif record_tamper == "secondary_context_event_binding":
                        record["current_presentation_established"]["video_frame_index"] += 1
                    elif record_tamper == "secondary_context_indices":
                        record["interleaved_readable_indices"] = [indices[1]]
                    elif record_tamper == "secondary_context_closure":
                        record["compatible_bar_intersections"] = [[2, 3]]
                    elif record_tamper == "secondary_optical_bracket":
                        record["left_support"][0]["video_frame_index"] += 1
                    elif record_tamper == "secondary_optical_established":
                        record["current_presentation_established"]["video_frame_index"] += 1
                    elif record_tamper == "secondary_optical_slot":
                        record["deficient_slot"] = 1
                    elif record_tamper == "secondary_optical_ocr":
                        record["raw_frequency_only_ocr"]["normalized_frequency"] = "35.500"
                    elif record_tamper == "secondary_optical_profile":
                        record["profile_sha256s"][0] = "not-a-digest"
                    elif record_tamper == "secondary_optical_profile_schema":
                        record["profile_schema"]["rows"] = 8
                    elif record_tamper == "secondary_optical_metrics":
                        record["profile_metrics"]["maximum_target_support_rms"] = 5.0
                    elif record_tamper == "secondary_optical_limits":
                        record["profile_limits"]["maximum_target_support_rms"] = 5.0
                    elif record_tamper == "secondary_optical_event_signature":
                        record["event_signature"]["current_secondary"] = []
                    elif record_tamper == "reader_binding":
                        record["reader_sha256"] = "b" * 64
                    elif record_tamper == "selection_binding":
                        record["selection_manifest_sha256"] = digest(selection_path)
                    elif record_tamper == "extra_field":
                        record["contradictory_extra_field"] = True
                admitted_records.append(record)
                claim_matches = (visually_eligible and not claim_mismatch_item
                                 and not (observer_claim_tamper and index == 0))
                code = None
            else:
                record = {
                    "event_id": event_id,
                    "classifier_id": classifier,
                    "field": affected[0],
                    **({"branch": branch} if frequency_context else {}),
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
            if clip_mapping_tamper == "hidden_full_position" and index == 0:
                hidden_items[-1]["full_run_clip_frame_indices"][0] += 1
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
                "path_matches_id": True,
                "source_frame_indices_match_target_run": True,
                "target_run_inside_clip": True,
            })

        if extra_rejected_candidates:
            # The analysis and frozen result keep the entire candidate inventory;
            # only the observer packet and its comparison are sampled.
            selected_rejections = encounter_qualification.select_temporal_rejections(
                classifier, capture_id, rejected_records)
            if selection_tamper == "all_rejections":
                selected_rejections = rejected_records
            elif selection_tamper == "reselected_rejection":
                excluded = next(record for record in rejected_records
                                if record not in selected_rejections)
                selected_rejections[-1] = excluded
            selected_records = [*admitted_records, *selected_rejections]
            selected_admitted_count = len(admitted_records)
            if selection_tamper == "omitted_admission":
                selected_records.remove(admitted_records[-1])
                selected_admitted_count -= 1
            selected_hashes = {canonical_digest(record) for record in selected_records}
            selected_ids = {item["opaque_id"] for item in hidden_items
                            if item["frozen_classifier_record_sha256"] in selected_hashes}
            manifest_items = [item for item in manifest_items if item["opaque_id"] in selected_ids]
            hidden_items = [item for item in hidden_items if item["opaque_id"] in selected_ids]
            observations = [item for item in observations if item["opaque_id"] in selected_ids]
            comparisons = [item for item in comparisons if item["opaque_id"] in selected_ids]
            clip_checks = [item for item in clip_checks if item["opaque_id"] in selected_ids]
            outcome_ids = {name: [item["opaque_id"] for item in comparisons
                                  if item["comparison_outcome"].casefold() == name]
                           for name in outcome_ids}
            ground_truth_counts = Counter(item["observer_ground_truth"] for item in comparisons)
            self.write_json(f"{temporal_root.name}/sources/selection.json",
                            encounter_qualification.temporal_selection_document(
                                selected_admitted_count, len(rejected_records),
                                [item["opaque_id"] for item in manifest_items]))

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
                "candidate_selection": copy.deepcopy(encounter_qualification.TEMPORAL_CANDIDATE_SELECTION),
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
                        "maximum_source_interval_ns": recording_maximum_interval_ns,
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
                "sequence": {"events": analysis_events},
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
        branch_fields = {}
        branch_allowed = True
        if frequency_context:
            branch_counts = {
                branch: Counter() for branch in
                ("intact_mask",)}
            true_admit_bands = set()
            for comparison in comparisons:
                opaque_id = comparison["opaque_id"]
                outcome = comparison["comparison_outcome"]
                branch_counts[branch_by_opaque[opaque_id]][outcome.casefold()] += 1
                if outcome == "TRUE_ADMIT":
                    true_admit_bands.add(band_by_opaque[opaque_id])
            branch_matrices = {
                branch: {
                    **{name: counts[name] for name in outcome_ids},
                    "total": sum(counts.values()),
                }
                for branch, counts in branch_counts.items()
            }
            branch_minima = {
                branch: {
                    "required_true_admit_minimum": 5,
                    "required_true_reject_minimum": 5,
                    "required_false_admits": 0,
                    "observed_true_admit": values["true_admit"],
                    "observed_true_reject": values["true_reject"],
                    "observed_false_admit": values["false_admit"],
                    "true_admit_minimum_met": values["true_admit"] >= 5,
                    "true_reject_minimum_met": values["true_reject"] >= 5,
                    "false_admit_requirement_met": values["false_admit"] == 0,
                }
                for branch, values in branch_matrices.items()
            }
            required_bands = ["X", "K", "Ka"]
            band_coverage = [band for band in required_bands if band in true_admit_bands]
            branch_allowed = (
                all(values["false_admit"] == 0
                    and values["true_admit"] >= 5 and values["true_reject"] >= 5
                    for values in branch_matrices.values())
                and band_coverage == required_bands)
            branch_fields = {
                "branch_confusion_matrices": branch_matrices,
                "branch_numerical_minima": branch_minima,
                "true_admit_band_coverage": band_coverage,
                "required_band_coverage_met": band_coverage == required_bands,
            }
        coverage_allowed = True
        if secondary_optical:
            true_admit_bands = {
                band_by_opaque[comparison["opaque_id"]]
                for comparison in comparisons
                if comparison["comparison_outcome"] == "TRUE_ADMIT"}
            required_bands = ["X", "K", "Ka"]
            band_coverage = [band for band in required_bands if band in true_admit_bands]
            coverage_allowed = band_coverage == required_bands
            branch_fields = {
                "true_admit_band_coverage": band_coverage,
                "required_band_coverage_met": coverage_allowed,
            }
        allowed = (matrix["false_admit"] == 0 and matrix["true_admit"] >= 5
                   and matrix["true_reject"] >= 5 and branch_allowed and coverage_allowed)
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
            **branch_fields,
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
            "implementation_sha256": {
                name: self.method[name]
                for name in CLASSIFIER_IMPLEMENTATION_FILES[classifier]
            },
            "spec": self.reference(spec, self.root),
            "validation": self.reference(comparison_path, self.root),
            "source_artifacts": relative_refs,
        }
        policy_spec = {
            "classifier_spec_sha256": spec_sha,
            "deadline_observation_semantics":
                spec_document["deadline_observation_semantics"],
            "raw_affected_fields": copy.deepcopy(
                TEMPORAL_V2_OBSERVER_RUBRICS[classifier]["raw_affected_fields"]),
        }
        if "auxiliary_closure_context_ns" in spec_document:
            policy_spec["auxiliary_closure_context_ns"] = spec_document["auxiliary_closure_context_ns"]
        if "verification_closure_semantics" in spec_document:
            policy_spec["verification_closure_semantics"] = spec_document[
                "verification_closure_semantics"]
        self.policy = {"contract_version": 3, "qualified_temporal_classifiers": {
            classifier: policy_spec}}
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
                "implementation_sha256": {
                    name: self.method[name] for name in STATIC_READER_IMPLEMENTATION_FILES
                },
                "runtime": copy.deepcopy(READER),
            },
            "camera": copy.deepcopy(CAMERA),
            "field_validation": self.reference(field_path, self.root),
            "visible_secondary_validation": self.reference(secondary_path, self.root),
            "fault_controls": self.reference(controls_path, self.root),
            "temporal_classifiers": temporal,
        }
        return self.write_json("qualification.json", manifest)

    def verify(self, path, *, source_gap_at_index=None):
        def regenerated(image_path, _registration):
            return copy.deepcopy(self.reader_observations[digest(image_path)])

        def retained_rows(_classifier_id, _paths, _sources, _window, _selection, _items):
            return {index: {
                "source_frame_seq": 1000 + index,
                "capture_ns": 1_000_000_000 + index * 5_000_000 + (
                    10_000_000 if source_gap_at_index is not None
                    and index >= source_gap_at_index else 0),
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

    def test_probe_image_drift_or_invalid_source_rejects_qualification(self):
        path = self.write_bundle()
        with patch("encounter_runtime_probe.probe_image_sha256", return_value="0" * 64):
            result = self.verify(path)
            self.assertEqual(result["status"], "REJECTED")
            self.assertIn("probe identity differs", result["errors"][0])
        for error in (OSError("missing"), ValueError("invalid encoding")):
            with patch("encounter_runtime_probe.probe_image_sha256", side_effect=error):
                result = self.verify(path)
                self.assertEqual(result["status"], "REJECTED")
                self.assertIn("probe source is unavailable or invalid", result["errors"][0])

    def test_complete_temporal_bundle_qualifies(self):
        temporal, _ = self.temporal_validation()
        result = self.verify(self.write_bundle(temporal=temporal))
        self.assertEqual(result["status"], "QUALIFIED")
        self.assertEqual(result["temporal_classifiers"]["v1-arrow-phase-edge-v2"]["total"], 10)

    def test_each_generic_temporal_v2_bundle_qualifies(self):
        classifiers = (
            "v1-arrow-phase-edge-v5",
            "v1-arrow-target-acquisition-v1",
            "v1-stable-frequency-intact-context-v1",
            "v1-secondary-closed-context-v3",
            "v1-main-bar-adjacent-redraw-v2",
            "v1-muted-badge-rising-fill-v2",
            "v1-unmute-stable-frequency-sweep-v2",
        )
        for classifier in classifiers:
            with self.subTest(classifier=classifier):
                temporal, _ = self.generic_temporal_validation(classifier)
                result = self.verify(self.write_bundle(temporal=temporal))
                self.assertEqual(result["status"], "QUALIFIED", result["errors"])
                expected_total = 10
                self.assertEqual(
                    result["temporal_classifiers"][classifier]["total"], expected_total)

    def test_recording_gap_does_not_relax_each_admitted_support_chain(self):
        classifiers = set(TEMPORAL_V2_OBSERVER_RUBRICS) - {"v1-arrow-phase-edge-v5"}
        for classifier in classifiers:
            with self.subTest(classifier=classifier):
                temporal, _ = self.generic_temporal_validation(
                    classifier, recording_maximum_interval_ns=15_000_000)
                result = self.verify(self.write_bundle(temporal=temporal))
                self.assertEqual(result["status"], "QUALIFIED", result["errors"])

                # All record points and retained source times agree. The single
                # 15 ms gap is now inside the first admitted chain, whose total
                # span still fits. Its accurate recording-wide binding must not
                # make that local gap an acceptable transition observation.
                temporal, _ = self.generic_temporal_validation(
                    classifier, recording_maximum_interval_ns=15_000_000,
                    source_gap_at_index=11)
                result = self.verify(self.write_bundle(temporal=temporal),
                                     source_gap_at_index=11)
                self.assertEqual(result["status"], "REJECTED")
                self.assertIn("local source gap bound", result["errors"][0])

    def test_generic_classifiers_cannot_use_arrow_schema_v1(self):
        for classifier in TEMPORAL_V2_OBSERVER_RUBRICS:
            with self.subTest(classifier=classifier):
                temporal, _ = self.generic_temporal_validation(
                    classifier, schema_version=1)
                result = self.verify(self.write_bundle(temporal=temporal))
                self.assertEqual(result["status"], "REJECTED")
                self.assertIn("schema version 1 is unsupported", result["errors"][0])

    def test_generic_temporal_binds_every_dependency_in_each_freeze(self):
        classifier = "v1-main-bar-adjacent-redraw-v2"
        for artifact in ("seal", "pre_pixel_freeze", "frozen_classifier_result"):
            for name in CLASSIFIER_IMPLEMENTATION_FILES[classifier]:
                with self.subTest(artifact=artifact, implementation=name):
                    temporal, _ = self.generic_temporal_validation(
                        classifier, binding_tamper=artifact, binding_file=name)
                    result = self.verify(self.write_bundle(temporal=temporal))
                    self.assertEqual(result["status"], "REJECTED")
                    self.assertTrue(result["errors"])

    def test_generic_temporal_binds_reader_in_each_freeze(self):
        classifier = "v1-muted-badge-rising-fill-v2"
        for artifact in ("seal", "pre_pixel_freeze", "frozen_classifier_result"):
            with self.subTest(artifact=artifact):
                temporal, _ = self.generic_temporal_validation(
                    classifier, reader_tamper=artifact)
                result = self.verify(self.write_bundle(temporal=temporal))
                self.assertEqual(result["status"], "REJECTED")
                self.assertTrue(result["errors"])

    def test_generic_temporal_authenticates_blind_capture_boundary(self):
        classifier = "v1-main-bar-adjacent-redraw-v2"
        for tamper in ("capture_bench", "capture_source", "capture_window", "capture_pixels"):
            with self.subTest(tamper=tamper):
                temporal, _ = self.generic_temporal_validation(
                    classifier, reader_tamper=tamper)
                result = self.verify(self.write_bundle(temporal=temporal))
                self.assertEqual(result["status"], "REJECTED")
                self.assertIn("qualification capture", result["errors"][0])

    def test_generic_temporal_selection_is_complete_and_well_formed(self):
        classifier = "v1-main-bar-adjacent-redraw-v2"
        for tamper in ("reordered", "wrong_type"):
            with self.subTest(tamper=tamper):
                temporal, _ = self.generic_temporal_validation(
                    classifier, selection_tamper=tamper)
                result = self.verify(self.write_bundle(temporal=temporal))
                self.assertEqual(result["status"], "REJECTED")
                self.assertTrue(any(word in result["errors"][0]
                                    for word in ("selection differs", "source document")),
                                result["errors"])

    def test_generic_temporal_retains_full_inventory_but_scores_exact_selected_rejections(self):
        classifier = "v1-main-bar-adjacent-redraw-v2"
        temporal, context = self.generic_temporal_validation(
            classifier, extra_rejected_candidates=8)
        frozen = json.loads(context["source_paths"]["frozen_classifier_result"].read_text())
        selection = json.loads(context["source_paths"]["selection"].read_text())
        comparison = json.loads(context["comparison"].read_text())
        self.assertEqual(len(frozen["classifications"]), 5)
        self.assertEqual(len(frozen["rejected_runs"]), 13)
        self.assertEqual(selection["candidate_counts"], {"admitted": 5, "rejected": 13})
        self.assertEqual(selection["selected_counts"], {"admitted": 5, "rejected": 12})
        self.assertEqual(comparison["confusion_matrix"]["true_reject"], 12)
        self.assertEqual(comparison["confusion_matrix"]["total"], 17)
        self.assertEqual(self.verify(self.write_bundle(temporal=temporal))["status"], "QUALIFIED")

        for tamper, boundary in (
                ("all_rejections", "selection differs from the frozen candidate set"),
                ("omitted_admission", "selection differs from the frozen candidate set"),
                ("reselected_rejection", "hidden decisions differ from frozen classifier output")):
            with self.subTest(tamper=tamper):
                # The fixture recomputes packet counts, outcome denominators,
                # source hashes, the seal, and the comparison references.
                temporal, _ = self.generic_temporal_validation(
                    classifier, extra_rejected_candidates=8, selection_tamper=tamper)
                result = self.verify(self.write_bundle(temporal=temporal))
                self.assertEqual(result["status"], "REJECTED")
                self.assertIn(boundary, result["errors"][0])

    def test_generic_temporal_cannot_duplicate_one_candidate_to_meet_minima(self):
        classifier = "v1-main-bar-adjacent-redraw-v2"
        temporal, _ = self.generic_temporal_validation(
            classifier, duplicate_candidate=True)
        result = self.verify(self.write_bundle(temporal=temporal))
        self.assertEqual(result["status"], "REJECTED")
        self.assertIn("source candidate is duplicated", result["errors"][0])

    def test_generic_temporal_spec_cannot_contradict_supported_minima_or_rubric(self):
        classifier = "v1-main-bar-adjacent-redraw-v2"
        temporal, _ = self.generic_temporal_validation(
            classifier, contradictory_spec=True)
        result = self.verify(self.write_bundle(temporal=temporal))
        self.assertEqual(result["status"], "REJECTED")
        self.assertIn("specification validation contract differs", result["errors"][0])

    def test_generic_temporal_blind_protocol_and_malformed_record_reject_cleanly(self):
        classifier = "v1-main-bar-adjacent-redraw-v2"
        for options in ({"blind_protocol_tamper": True},
                        {"malformed_admitted_record": True}):
            with self.subTest(options=options):
                temporal, _ = self.generic_temporal_validation(classifier, **options)
                result = self.verify(self.write_bundle(temporal=temporal))
                self.assertEqual(result["status"], "REJECTED")
                self.assertTrue(result["errors"])

    def test_generic_temporal_validates_complete_classifier_record_invariants(self):
        cases = (
            ("v1-arrow-phase-edge-v5", "arrow_left_guard"),
            ("v1-arrow-phase-edge-v5", "arrow_right_guard"),
            ("v1-arrow-phase-edge-v5", "arrow_left_endpoint"),
            ("v1-arrow-phase-edge-v5", "arrow_right_endpoint"),
            ("v1-arrow-phase-edge-v5", "arrow_guard_sequence"),
            ("v1-arrow-phase-edge-v5", "arrow_guard_timestamp"),
            ("v1-arrow-phase-edge-v5", "arrow_inner_metrics"),
            ("v1-arrow-phase-edge-v5", "arrow_projection_bound"),
            ("v1-arrow-phase-edge-v5", "arrow_residual_bound"),
            ("v1-arrow-phase-edge-v5", "arrow_endpoint_span"),
            ("v1-arrow-target-acquisition-v1", "acquisition_phase"),
            ("v1-arrow-target-acquisition-v1", "acquisition_motion"),
            ("v1-arrow-target-acquisition-v1", "acquisition_unchanged"),
            ("v1-arrow-target-acquisition-v1", "acquisition_claim_order"),
            ("v1-arrow-target-acquisition-v1", "acquisition_claim_state_keys"),
            ("v1-arrow-target-acquisition-v1", "acquisition_claim_noncurrent"),
            ("v1-stable-frequency-intact-context-v1", "frequency_context_branch"),
            ("v1-stable-frequency-intact-context-v1", "frequency_context_masks"),
            ("v1-stable-frequency-intact-context-v1", "frequency_context_partial"),
            ("v1-stable-frequency-intact-context-v1", "frequency_context_closure"),
            ("v1-secondary-closed-context-v3", "secondary_context_established"),
            ("v1-secondary-closed-context-v3", "secondary_context_event_binding"),
            ("v1-secondary-closed-context-v3", "secondary_context_indices"),
            ("v1-secondary-closed-context-v3", "secondary_context_closure"),
            ("v1-secondary-text-optical-bridge-v1", "secondary_optical_bracket"),
            ("v1-secondary-text-optical-bridge-v1", "secondary_optical_established"),
            ("v1-secondary-text-optical-bridge-v1", "secondary_optical_slot"),
            ("v1-secondary-text-optical-bridge-v1", "secondary_optical_ocr"),
            ("v1-secondary-text-optical-bridge-v1", "secondary_optical_profile"),
            ("v1-secondary-text-optical-bridge-v1", "secondary_optical_profile_schema"),
            ("v1-secondary-text-optical-bridge-v1", "secondary_optical_metrics"),
            ("v1-secondary-text-optical-bridge-v1", "secondary_optical_limits"),
            ("v1-secondary-text-optical-bridge-v1", "secondary_optical_event_signature"),
            ("v1-main-bar-adjacent-redraw-v2", "bar_changed_index"),
            ("v1-main-bar-adjacent-redraw-v2", "bar_expectation"),
            ("v1-main-bar-adjacent-redraw-v2", "reader_binding"),
            ("v1-main-bar-adjacent-redraw-v2", "selection_binding"),
            ("v1-main-bar-adjacent-redraw-v2", "extra_field"),
            ("v1-main-bar-adjacent-redraw-v2", "analysis_result"),
            ("v1-muted-badge-rising-fill-v2", "badge_direction"),
            ("v1-muted-badge-rising-fill-v2", "stable_fields"),
            ("v1-muted-badge-rising-fill-v2", "support_sequence"),
            ("v1-unmute-stable-frequency-sweep-v2", "frequency_masks"),
            ("v1-unmute-stable-frequency-sweep-v2", "support_timestamp"),
            ("v1-unmute-stable-frequency-sweep-v2", "rejected_field"),
        )
        for classifier, tamper in cases:
            with self.subTest(classifier=classifier, tamper=tamper):
                temporal, _ = self.generic_temporal_validation(
                    classifier, record_tamper=tamper)
                result = self.verify(self.write_bundle(temporal=temporal))
                self.assertEqual(result["status"], "REJECTED")
                self.assertTrue(result["errors"])

    def test_generic_temporal_derives_clip_path_identity_and_exact_instructions(self):
        classifier = "v1-main-bar-adjacent-redraw-v2"
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
                self.assertNotIn("full_run", instructions)
                self.assertIn("same classifier-independent rule", instructions)
                self.assertIn("within 300 ms before", instructions)
                self.assertIn("authenticated capture timing", instructions)
                self.assertIn("Do not make that attestation if it is not true", instructions)
                self.assertIn("Allowed:", instructions)
                self.assertIn("Eligibility:", instructions)
                for field in rubric["literal_fields"]:
                    self.assertIn(f"- {field}:", instructions)
                for allowed in rubric["allowed_literals"].values():
                    for literal in allowed:
                        self.assertIn(literal, instructions)
        arrow = temporal_v2_observer_instructions("v1-arrow-phase-edge-v5")
        for requirement in (
                "first and last zero-based target clip-frame indices",
                "[a-3,a-2]", "[b+2,b+3]", "[a-3,b+3]", "a-1 and b+1",
                "Do not search farther", "[] is a valid clear endpoint",
                "equal brightness is not required", "Missing or unclear support"):
            self.assertIn(requirement, arrow)
        badge = temporal_v2_observer_instructions("v1-muted-badge-rising-fill-v2")
        self.assertIn("Uniform palette or brightness recoloring caused by mute is allowed", badge)
        self.assertIn("frequency, band, direction, bar geometry or count", badge)
        acquisition = temporal_v2_observer_instructions(
            "v1-arrow-target-acquisition-v1")
        for requirement in (
                "BOTH_CLEAR", "PRIOR_OR_PRIOR_PLUS_CURRENT_TO_CURRENT",
                "COHERENT_CHANGED_DIRECTION_MOTION",
                "EVERY_CLAIMED_FRAME_HAS_NONCURRENT_CHANGED_DIRECTION",
                "unchanged_direction_motion", "HIGH"):
            self.assertIn(requirement, acquisition)
        frequency = temporal_v2_observer_instructions(
            "v1-stable-frequency-intact-context-v1")
        for requirement in (
                "SAME_FREQUENCY_GLYPHS_THROUGHOUT", "BOTH_CLEAR",
                "LEGAL_TARGET_CONTENT", "HIGH", "Do not infer or record the machine branch"):
            self.assertIn(requirement, frequency)
        secondary = temporal_v2_observer_instructions(
            "v1-secondary-closed-context-v3")
        for requirement in (
                "SAME_CURRENT_CARD_CONTEXT", "BOTH_CLEAR",
                "COHERENT_PARTIAL_METER_REDRAW", "EXACT_COUNT_CLOSURE", "HIGH"):
            self.assertIn(requirement, secondary)
        optical = temporal_v2_observer_instructions(
            "v1-secondary-text-optical-bridge-v1")
        for requirement in (
                "BOTH_CLEAR", "CENTER_COMPLETE_TEXT_CLEAR",
                "SAME_COMPLETE_CARD_TEXT_THROUGHOUT",
                "NO_DIRECTION_OR_METER_CHANGE", "HIGH"):
            self.assertIn(requirement, optical)

    def test_frequency_context_band_uses_actual_decoded_wire_row_schema(self):
        from encounter_expectation import _row
        from test_encounter_expectation import packet

        for band, mask, mhz in (("X", 8, 10525), ("K", 4, 24150), ("Ka", 2, 34700)):
            row = _row(packet(0x43, [0x11, mhz >> 8, mhz & 255, 1, 1, mask | 32, 128]))
            frequency = row["frequency"]
            record = {"event_id": "event-0001", "support_derived_frequency": frequency,
                      "event_signature": {"mode": "CHANGED", "changed_fields": ["primary_frequency"],
                                          "current_primary_frequency": frequency}}
            event = {"event_id": "event-0001", "mode": "CHANGED", "changed_fields": ["primary_frequency"],
                     "wire_rows": [row], "target": {"fields": {"primary_frequency": {"allowed": [frequency]}}}}
            result = {"sequence": {"events": [event]}}
            self.assertEqual(encounter_qualification._temporal_v2_frequency_context_band(record, result), band)
            row["frequency"] = "10.526"
            with self.assertRaisesRegex(encounter_qualification.QualificationError,
                                        "differs from its retained primary event"):
                encounter_qualification._temporal_v2_frequency_context_band(record, result)

    def test_frequency_context_requires_intact_minima_and_all_bands(self):
        classifier = "v1-stable-frequency-intact-context-v1"
        cases = (
            {"frequency_below_minimum": True},
            {"false_admit": True},
            {"frequency_missing_band": True},
        )
        for options in cases:
            with self.subTest(options=options):
                temporal, _ = self.generic_temporal_validation(classifier, **options)
                result = self.verify(self.write_bundle(temporal=temporal))
                self.assertEqual(result["status"], "REJECTED")
                self.assertIn("integrity failed", result["errors"][0])

    def test_frequency_context_strata_tampering_is_rejected(self):
        classifier = "v1-stable-frequency-intact-context-v1"
        temporal, context = self.generic_temporal_validation(classifier)
        self.rewrite_temporal_comparison(
            temporal, context,
            lambda document: document["branch_confusion_matrices"]["intact_mask"].update(
                true_admit=6))
        result = self.verify(self.write_bundle(temporal=temporal))
        self.assertEqual(result["status"], "REJECTED")
        self.assertIn("strata were not independently derived", result["errors"][0])

        temporal, context = self.generic_temporal_validation(classifier)
        self.rewrite_temporal_comparison(
            temporal, context,
            lambda document: document.update(true_admit_band_coverage=["X", "K", "Ka", "Ku"]))
        result = self.verify(self.write_bundle(temporal=temporal))
        self.assertEqual(result["status"], "REJECTED")
        self.assertIn("strata were not independently derived", result["errors"][0])

    def test_frequency_context_observer_must_see_complete_segments(self):
        classifier = "v1-stable-frequency-intact-context-v1"
        for integrity, eligible in (("ALL_SEGMENTS_COMPLETE", True),
                                    ("PARTIAL_OR_MISSING_SEGMENT", False),
                                    ("INDETERMINATE", False)):
            with self.subTest(integrity=integrity):
                observation = {
                    "opaque_id": "opaque",
                    **self.generic_temporal_literal(classifier, True),
                    "observed_frequency": "34.700",
                    "segment_integrity": integrity,
                }
                _, observed_eligible = encounter_qualification._temporal_v2_observer_result(
                    classifier, observation)
                self.assertEqual(observed_eligible, eligible)

    def test_frequency_context_spec_and_rejection_contract_are_exact(self):
        classifier = "v1-stable-frequency-intact-context-v1"
        temporal, _ = self.generic_temporal_validation(classifier)
        self.policy["qualified_temporal_classifiers"][classifier][
            "verification_closure_semantics"] = "INVENTED_CLOSURE"
        result = self.verify(self.write_bundle(temporal=temporal))
        self.assertEqual(result["status"], "REJECTED")
        self.assertIn("policy contract differs", result["errors"][0])

        temporal, _ = self.generic_temporal_validation(
            classifier, contradictory_spec=True)
        result = self.verify(self.write_bundle(temporal=temporal))
        self.assertEqual(result["status"], "REJECTED")
        self.assertIn("specification validation contract differs", result["errors"][0])

        temporal, _ = self.generic_temporal_validation(
            classifier, rejection_code="SUPPORT_GEOMETRY")
        self.assertEqual(
            self.verify(self.write_bundle(temporal=temporal))["status"], "QUALIFIED")
        temporal, _ = self.generic_temporal_validation(
            classifier, rejection_code="INVENTED_REJECTION")
        result = self.verify(self.write_bundle(temporal=temporal))
        self.assertEqual(result["status"], "REJECTED")
        self.assertIn("rejected classifier record shape differs", result["errors"][0])

    def test_arrow_acquisition_spec_semantics_and_rejection_codes_are_exact(self):
        classifier = "v1-arrow-target-acquisition-v1"
        temporal, _ = self.generic_temporal_validation(
            classifier, contradictory_spec=True)
        result = self.verify(self.write_bundle(temporal=temporal))
        self.assertEqual(result["status"], "REJECTED")
        self.assertIn("specification validation contract differs", result["errors"][0])

        temporal, _ = self.generic_temporal_validation(
            classifier, rejection_code="UNCHANGED_DIRECTION_MOTION")
        self.assertEqual(
            self.verify(self.write_bundle(temporal=temporal))["status"], "QUALIFIED")
        temporal, _ = self.generic_temporal_validation(
            classifier, rejection_code="INVENTED_REJECTION")
        result = self.verify(self.write_bundle(temporal=temporal))
        self.assertEqual(result["status"], "REJECTED")
        self.assertIn("rejected classifier record shape differs", result["errors"][0])

    def test_secondary_context_spec_and_rejection_codes_are_exact(self):
        classifier = "v1-secondary-closed-context-v3"
        for field, value in (("verification_closure_semantics", "INVENTED_CLOSURE"),
                             ("auxiliary_closure_context_ns", 80_000_001)):
            with self.subTest(field=field):
                temporal, _ = self.generic_temporal_validation(classifier)
                self.policy["qualified_temporal_classifiers"][classifier][field] = value
                result = self.verify(self.write_bundle(temporal=temporal))
                self.assertEqual(result["status"], "REJECTED")
                self.assertIn("policy contract differs", result["errors"][0])
        temporal, _ = self.generic_temporal_validation(
            classifier, contradictory_spec=True)
        result = self.verify(self.write_bundle(temporal=temporal))
        self.assertEqual(result["status"], "REJECTED")
        self.assertIn("specification validation contract differs", result["errors"][0])

        temporal, _ = self.generic_temporal_validation(
            classifier, rejection_code="METER_COMPATIBILITY")
        self.assertEqual(
            self.verify(self.write_bundle(temporal=temporal))["status"], "QUALIFIED")
        temporal, _ = self.generic_temporal_validation(
            classifier, rejection_code="INVENTED_REJECTION")
        result = self.verify(self.write_bundle(temporal=temporal))
        self.assertEqual(result["status"], "REJECTED")
        self.assertIn("rejected classifier record shape differs", result["errors"][0])

    def test_secondary_optical_requires_all_bands_and_zero_false_admits(self):
        classifier = "v1-secondary-text-optical-bridge-v1"
        for options in ({"optical_missing_band": True}, {"false_admit": True}):
            with self.subTest(options=options):
                temporal, _ = self.generic_temporal_validation(classifier, **options)
                result = self.verify(self.write_bundle(temporal=temporal))
                self.assertEqual(result["status"], "REJECTED")
                self.assertIn("integrity failed", result["errors"][0])

    def test_secondary_optical_band_coverage_tampering_is_rejected(self):
        classifier = "v1-secondary-text-optical-bridge-v1"
        temporal, context = self.generic_temporal_validation(classifier)
        self.rewrite_temporal_comparison(
            temporal, context,
            lambda document: document.update(true_admit_band_coverage=["X", "K", "Ka", "Ku"]))
        result = self.verify(self.write_bundle(temporal=temporal))
        self.assertEqual(result["status"], "REJECTED")
        self.assertIn("band coverage was not independently derived", result["errors"][0])

    def test_secondary_optical_spec_and_rejection_codes_are_exact(self):
        classifier = "v1-secondary-text-optical-bridge-v1"
        repository_spec = (Path(__file__).resolve().parent / "bench" / "temporal_specs" /
                           f"{classifier}.json")
        self.assertEqual(digest(repository_spec),
                         "f8bbbb1cb962ed404143d7e2a3a0ef05e4c68eeb1adc5c069b284b82ac40060a")
        temporal, _ = self.generic_temporal_validation(classifier, contradictory_spec=True)
        result = self.verify(self.write_bundle(temporal=temporal))
        self.assertEqual(result["status"], "REJECTED")
        self.assertIn("specification validation contract differs", result["errors"][0])

        temporal, _ = self.generic_temporal_validation(
            classifier, rejection_code="OPTICAL_DIFFERENCE")
        self.assertEqual(
            self.verify(self.write_bundle(temporal=temporal))["status"], "QUALIFIED")
        temporal, _ = self.generic_temporal_validation(
            classifier, rejection_code="INVENTED_REJECTION")
        result = self.verify(self.write_bundle(temporal=temporal))
        self.assertEqual(result["status"], "REJECTED")
        self.assertIn("rejected classifier record shape differs", result["errors"][0])

    def test_secondary_optical_owner_and_probe_drift_are_rejected(self):
        classifier = "v1-secondary-text-optical-bridge-v1"
        temporal, _ = self.generic_temporal_validation(classifier)
        path = self.write_bundle(temporal=temporal)
        for name in ("encounter_secondary_optical_bridge.py", "encounter_secondary_probe.py"):
            with self.subTest(name=name):
                self.method[name] = "c" * 64
                try:
                    result = self.verify(path)
                finally:
                    self.method[name] = SHA
                self.assertEqual(result["status"], "REJECTED")
                self.assertIn(
                    f"running temporal implementation differs: {name}", result["errors"][0])

    def test_policy_classifier_contract_must_exactly_match_spec_and_rubric(self):
        classifier = "v1-secondary-closed-context-v3"
        mutations = (
            lambda value: value.update(raw_affected_fields=["main_arrows"]),
            lambda value: value.update(
                deadline_observation_semantics="TARGET_ACQUISITION_TRANSITION"),
            lambda value: value.update(unrecognized_policy_claim=True),
        )
        for contract_version in (2, 3):
            for mutate in mutations:
                with self.subTest(contract_version=contract_version, mutation=mutate):
                    temporal, _ = self.generic_temporal_validation(classifier)
                    self.policy["contract_version"] = contract_version
                    mutate(self.policy["qualified_temporal_classifiers"][classifier])
                    result = self.verify(self.write_bundle(temporal=temporal))
                    self.assertEqual(result["status"], "REJECTED")
                    self.assertIn("policy contract differs", result["errors"][0])

    def test_legacy_v1_policy_classifier_shape_remains_exact(self):
        temporal, context = self.temporal_validation()
        classifier = context["classifier"]
        self.assertEqual(
            self.verify(self.write_bundle(temporal=temporal))["status"], "QUALIFIED")
        self.policy["qualified_temporal_classifiers"][classifier][
            "deadline_observation_semantics"] = "LEGAL_PRESENTATION_TRANSITION"
        result = self.verify(self.write_bundle(temporal=temporal))
        self.assertEqual(result["status"], "REJECTED")
        self.assertIn("policy contract differs", result["errors"][0])

    def test_policy_classifier_spec_hash_must_match_qualified_spec(self):
        classifier = "v1-secondary-closed-context-v3"
        temporal, _ = self.generic_temporal_validation(classifier)
        self.policy["qualified_temporal_classifiers"][classifier][
            "classifier_spec_sha256"] = "b" * 64
        result = self.verify(self.write_bundle(temporal=temporal))
        self.assertEqual(result["status"], "REJECTED")
        self.assertIn("temporal specification differs", result["errors"][0])

    def test_generic_temporal_binds_clip_frame_mapping(self):
        classifier = "v1-main-bar-adjacent-redraw-v2"
        for tamper in ("source_gap", "target_position", "hidden_full_position"):
            with self.subTest(tamper=tamper):
                temporal, _ = self.generic_temporal_validation(
                    classifier, clip_mapping_tamper=tamper)
                result = self.verify(self.write_bundle(temporal=temporal))
                self.assertEqual(result["status"], "REJECTED")
                self.assertIn("mapping", result["errors"][0])

    def test_target_observer_transcription_must_match_the_frozen_visible_claim(self):
        classifiers = (
            "v1-arrow-phase-edge-v5",
            "v1-arrow-target-acquisition-v1",
            "v1-stable-frequency-intact-context-v1",
            "v1-secondary-closed-context-v3",
            "v1-secondary-text-optical-bridge-v1",
        )
        for classifier in classifiers:
            with self.subTest(classifier=classifier):
                temporal, context = self.generic_temporal_validation(
                    classifier, observer_claim_tamper=True)
                comparison = json.loads(context["comparison"].read_text(encoding="utf-8"))
                self.assertEqual(comparison["confusion_matrix"]["false_admit"], 1)
                result = self.verify(self.write_bundle(temporal=temporal))
                self.assertEqual(result["status"], "REJECTED")

    def test_target_observer_transcription_schema_is_exact(self):
        valid_card = copy.deepcopy(CARD)
        cases = (
            ("v1-arrow-phase-edge-v5", {
                "left_endpoint_directions": [],
                "right_endpoint_directions": ["side", "front"],
            }),
            ("v1-arrow-target-acquisition-v1", {
                "left_endpoint_directions": ["front", "side"],
                "right_endpoint_directions": ["invented"],
            }),
            ("v1-stable-frequency-intact-context-v1", {
                "observed_frequency": "34.7",
            }),
            ("v1-secondary-closed-context-v3", {
                "observed_cards": [{**valid_card, "extra": True}],
            }),
            ("v1-secondary-text-optical-bridge-v1", {
                "support_cards": [valid_card],
                "center_cards": [{**valid_card, "bars": 7}],
            }),
        )
        for classifier, mutation in cases:
            with self.subTest(classifier=classifier):
                observation = {
                    "opaque_id": "opaque",
                    **self.generic_temporal_literal(classifier, True),
                    **mutation,
                }
                for name in TEMPORAL_V2_OBSERVER_RUBRICS[classifier]["literal_fields"]:
                    observation.setdefault(name, None)
                with self.assertRaises(encounter_qualification.QualificationError):
                    encounter_qualification._temporal_v2_observer_result(
                        classifier, observation)

    def test_arrow_transcription_uses_visual_direction_order_without_losing_set_identity(self):
        classifier = "v1-arrow-phase-edge-v5"
        literal = {
            **self.generic_temporal_literal(classifier, True),
            "left_endpoint_directions": ["front", "rear"],
            "right_endpoint_directions": ["front", "side", "rear"],
        }
        record = {
            "classifier_id": classifier,
            "classifier_spec_sha256": "a" * 64,
            "status": "QUALIFIED_CAPTURE_TRANSITION",
            "raw_affected_fields": ["main_arrows"],
            # Classifier records use Python lexical ordering.
            "endpoint_values": [["front", "rear"], ["front", "rear", "side"]],
        }
        self.assertTrue(encounter_qualification._temporal_v2_claim_matches_record(
            classifier, "a" * 64, literal, record))

    def test_generic_mute_rejection_contract_includes_event_scope(self):
        for classifier in ("v1-muted-badge-rising-fill-v2",
                           "v1-unmute-stable-frequency-sweep-v2"):
            with self.subTest(classifier=classifier):
                temporal, _ = self.generic_temporal_validation(
                    classifier, rejection_code="EVENT_SCOPE")
                result = self.verify(self.write_bundle(temporal=temporal))
                self.assertEqual(result["status"], "QUALIFIED", result["errors"])

    def test_generic_temporal_binds_code_owned_rubric(self):
        classifier = "v1-unmute-stable-frequency-sweep-v2"
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
        classifier = "v1-main-bar-adjacent-redraw-v2"
        for name, mutation in mutations.items():
            with self.subTest(tamper=name):
                temporal, context = self.generic_temporal_validation(classifier)
                self.rewrite_temporal_comparison(temporal, context, mutation)
                result = self.verify(self.write_bundle(temporal=temporal))
                self.assertEqual(result["status"], "REJECTED")
                self.assertIn("independently derived", result["errors"][0])

    def test_generic_temporal_false_admission_is_rejected_with_minima_met(self):
        cases = (
            ("v1-arrow-target-acquisition-v1", {"false_admit": True}),
            ("v1-secondary-closed-context-v3", {"false_admit": True}),
            ("v1-main-bar-adjacent-redraw-v2", {"false_admit": True}),
            ("v1-unmute-stable-frequency-sweep-v2", {"claim_mismatch": True}),
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
                expected_rejections = 5
                self.assertEqual(comparison["confusion_matrix"]["true_reject"], 0)
                self.assertEqual(
                    comparison["confusion_matrix"]["abstain"], expected_rejections)
                self.assertEqual(
                    comparison["denominators"]["observer_indeterminate"], expected_rejections)
                self.assertFalse(comparison["numerical_minima"]["true_reject_minimum_met"])
                self.assertTrue(all(
                    item["observer_ground_truth"] == "INDETERMINATE"
                    and item["comparison_outcome"] == "ABSTAIN"
                    for item in comparison["comparisons"]
                    if item["frozen_classifier_decision"] == "REJECTED"))
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
        classifier = "v1-main-bar-adjacent-redraw-v2"
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

    def test_each_static_dependency_drift_is_rejected(self):
        for name in STATIC_READER_IMPLEMENTATION_FILES:
            with self.subTest(name=name):
                path = self.write_bundle()
                original = self.method[name]
                self.method[name] = "c" * 64
                try:
                    result = self.verify(path)
                finally:
                    self.method[name] = original
                self.assertEqual(result["status"], "REJECTED")
                self.assertIn(f"running implementation differs: {name}", result["errors"][0])

    def test_static_qualification_ignores_non_static_implementation_drift(self):
        path = self.write_bundle()
        for name in (
                *COMMON_TEMPORAL_IMPLEMENTATION_FILES,
                "encounter_arrow_acquisition.py", "encounter_frequency_context.py",
                "encounter_secondary_context.py", "encounter_bar_transition.py",
                "encounter_report.py",
                "visible_event_policies.json"):
            with self.subTest(name=name):
                self.method[name] = "c" * 64
                self.assertEqual(self.verify(path)["status"], "QUALIFIED")

    def test_published_static_implementation_inventory_must_be_exact(self):
        mutations = (
            lambda binding: binding.pop("camera_contract.py"),
            lambda binding: binding.update({"encounter_report.py": SHA}),
        )
        for mutate in mutations:
            with self.subTest(mutation=mutate):
                path = self.write_bundle()
                manifest = json.loads(path.read_text(encoding="utf-8"))
                mutate(manifest["reader"]["implementation_sha256"])
                path.write_text(json.dumps(manifest), encoding="utf-8")
                result = self.verify(path)
                self.assertEqual(result["status"], "REJECTED")
                self.assertIn("exact static implementation inventory", result["errors"][0])

    def test_temporal_qualification_binds_owner_and_isolates_other_owners(self):
        cases = (
            ("v1-arrow-phase-edge-v5", "encounter_arrow_transition.py",
             "encounter_arrow_acquisition.py"),
            ("v1-arrow-target-acquisition-v1", "encounter_arrow_acquisition.py",
             "encounter_arrow_transition.py"),
        )
        for classifier, owned, unrelated in cases:
            with self.subTest(classifier=classifier):
                temporal, _ = self.generic_temporal_validation(classifier)
                path = self.write_bundle(temporal=temporal)

                self.method[unrelated] = "c" * 64
                self.assertEqual(self.verify(path)["status"], "QUALIFIED")
                self.method[owned] = "c" * 64
                result = self.verify(path)
                self.assertEqual(result["status"], "REJECTED")
                self.assertIn("running temporal implementation differs", result["errors"][0])
                self.method[owned] = SHA
                self.method[unrelated] = SHA

        classifier = "v1-stable-frequency-intact-context-v1"
        temporal, _ = self.generic_temporal_validation(classifier)
        path = self.write_bundle(temporal=temporal)
        self.method["encounter_arrow_transition.py"] = "c" * 64
        self.assertEqual(self.verify(path)["status"], "QUALIFIED")
        for owned in ("encounter_frequency_context.py", "encounter_redraw_probe.py"):
            self.method[owned] = "c" * 64
            result = self.verify(path)
            self.assertEqual(result["status"], "REJECTED")
            self.assertIn("running temporal implementation differs", result["errors"][0])
            self.method[owned] = SHA
        self.method["encounter_arrow_transition.py"] = SHA

    def test_every_classifier_binds_every_common_temporal_dependency(self):
        for classifier in TEMPORAL_V2_OBSERVER_RUBRICS:
            self.assertLessEqual(
                set(COMMON_TEMPORAL_IMPLEMENTATION_FILES),
                set(CLASSIFIER_IMPLEMENTATION_FILES[classifier]))
        classifier = "v1-arrow-phase-edge-v5"
        for name in COMMON_TEMPORAL_IMPLEMENTATION_FILES:
            with self.subTest(name=name):
                temporal, _ = self.generic_temporal_validation(classifier)
                path = self.write_bundle(temporal=temporal)
                self.method[name] = "c" * 64
                try:
                    result = self.verify(path)
                finally:
                    self.method[name] = SHA
                self.assertEqual(result["status"], "REJECTED")
                self.assertIn(
                    f"running temporal implementation differs: {name}", result["errors"][0])

    def test_temporal_implementation_inventory_must_be_exact(self):
        for classifier, owned in (
                ("v1-arrow-phase-edge-v5", "encounter_arrow_transition.py"),
                ("v1-arrow-target-acquisition-v1", "encounter_arrow_acquisition.py"),
                ("v1-stable-frequency-intact-context-v1", "encounter_frequency_context.py"),
                ("v1-stable-frequency-intact-context-v1", "encounter_redraw_probe.py"),
                ("v1-secondary-closed-context-v3", "encounter_secondary_context.py"),
                ("v1-secondary-closed-context-v3", "encounter_check.py"),
                ("v1-secondary-text-optical-bridge-v1",
                 "encounter_secondary_optical_bridge.py"),
                ("v1-secondary-text-optical-bridge-v1",
                 "encounter_secondary_probe.py")):
            mutations = (
                lambda binding, name=owned: binding.pop(name),
                lambda binding: binding.update({"encounter_bar_transition.py": SHA}),
            )
            for mutate in mutations:
                with self.subTest(classifier=classifier, mutation=mutate):
                    temporal, _ = self.generic_temporal_validation(classifier)
                    mutate(temporal[classifier]["implementation_sha256"])
                    result = self.verify(self.write_bundle(temporal=temporal))
                    self.assertEqual(result["status"], "REJECTED")
                    self.assertIn("temporal implementation inventory differs", result["errors"][0])

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

    def test_partial_secondary_identity_is_recomputed_from_exact_reader(self):
        secondary = self.visible_secondary_validation(reanalysis=True)
        item = secondary["items"][6]
        item["observed"]["partial_cards"][0]["frequency"] = "35.500"
        result = self.verify(self.write_bundle(secondary=secondary))
        self.assertIn("observation differs from exact reader", result["errors"][0])

    def test_wrong_partial_secondary_identity_is_rejected(self):
        secondary = self.visible_secondary_validation(wrong_partial=True)
        result = self.verify(self.write_bundle(secondary=secondary))
        self.assertIn("wrong partial identity", result["errors"][0])

    def test_partial_identity_without_resolved_blind_identity_is_rejected(self):
        secondary = self.visible_secondary_validation(unresolved_partial=True)
        result = self.verify(self.write_bundle(secondary=secondary))
        self.assertIn("partial secondary identity without a resolved blind reference",
                      result["errors"][0])

    def test_full_secondary_assertion_without_resolved_blind_card_is_rejected(self):
        secondary = self.visible_secondary_validation(unresolved_full_assertion=True)
        result = self.verify(self.write_bundle(secondary=secondary))
        self.assertIn("asserted visible-secondary content without a resolved blind reference",
                      result["errors"][0])

    def test_unknown_partial_secondary_identity_does_not_count_as_agreement(self):
        secondary = self.visible_secondary_validation(partial_identity_count=4)
        result = self.verify(self.write_bundle(secondary=secondary))
        self.assertIn("too few blind agreements on partial secondary identities",
                      result["errors"][0])

    def test_partial_secondary_identity_on_blind_empty_control_is_rejected(self):
        document = self.field_validation(ghost_partial_identity=True)
        result = self.verify(self.write_bundle(document))
        self.assertIn("partial secondary identity on a blind empty control",
                      result["errors"][0])

    def test_too_few_blind_empty_secondary_controls_are_rejected(self):
        document = self.field_validation(empty_control_count=4)
        result = self.verify(self.write_bundle(document))
        self.assertIn("too few blind empty-secondary presence controls", result["errors"][0])

    def test_complete_old_reader_reanalysis_is_explicitly_bound(self):
        secondary = self.visible_secondary_validation(reanalysis=True)
        result = self.verify(self.write_bundle(secondary=secondary))
        self.assertEqual(result["status"], "QUALIFIED")
        self.assertTrue(result["visible_secondary_validation"]["reader_reanalysis"])
        secondary["reader_reanalysis"]["source_reader_sha256"] = "e" * 64
        result = self.verify(self.write_bundle(secondary=secondary))
        self.assertIn("reanalysis binding differs", result["errors"][0])

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
