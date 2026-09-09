"""Additional independent card observations; the historical packet stays intact."""
from __future__ import annotations

from collections import Counter
from copy import deepcopy
import json
from pathlib import Path
import re
import shutil

from encounter_primary_frequency_reference import _artifact, _item_registration, _sha

BLIND_PROTOCOL = {
    "labels_completed_before_key_access": True,
    "observer_received_machine_output": False,
    "observer_received_hidden_key": False,
}
ROLES = {"development_original", "held_out_original"}


def _require(condition, message):
    if not condition:
        raise ValueError("secondary reference: " + message)


def _read(path):
    return json.loads(path.read_bytes())


def _indexed(records, name):
    _require(isinstance(records, list), name + " items are missing")
    indexed = {}
    for record in records:
        _require(isinstance(record, dict) and isinstance(record.get("id"), str)
                 and record["id"] not in indexed, name + " identities are missing or duplicated")
        indexed[record["id"]] = record
    return indexed


def _packet(path):
    document = _read(path)
    _require(document.get("schema_version") == 1
             and document.get("kind") == "independent_secondary_reference", "unsupported supplement")
    manifest_path = _artifact(path.parent, document.get("blind_manifest"))
    manifest = _read(manifest_path)
    records = _indexed(manifest.get("items"), "blind manifest")
    return document, manifest_path, {key: value for key, value in records.items()
                                    if value.get("field") == "secondary"}


def copy_reference(source, destination):
    """Copy the complete secondary subset and all retained provenance by hash."""
    source, destination = Path(source).resolve(), Path(destination)
    document, manifest_path, packet = _packet(source)
    refs = [document[name] for name in
            ("protocol", "blind_manifest", "observations", "selection_before_reading")]
    refs += document.get("provenance_artifacts", [])
    for item in document["items"]:
        refs.append(item["image"])
        refs += [item["startup_calibration"][name] for name in ("preflight", "still")]
    sources = [(_artifact(source.parent, ref), Path(ref["path"])) for ref in refs]
    for record in packet.values():
        image = _artifact(manifest_path.parent, {"path": record.get("image"), "sha256": record.get("sha256")})
        sources.append((image, image.relative_to(source.parent)))
    destination.parent.mkdir(parents=True, exist_ok=True)
    for original, relative in sources:
        target = destination.parent / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        _require(not target.exists() or _sha(target) == _sha(original), "copied artifact path collision")
        shutil.copyfile(original, target)
    shutil.copyfile(source, destination)


def reference_reread_binding(path, method):
    """Keep the original candidate freeze when a later reader uses these labels."""
    from encounter_qualification import CORE_READER_FILES
    path = Path(path).resolve()
    frozen = _read(path).get("frozen_reader_files")
    current = {name: method.get(name) for name in CORE_READER_FILES}
    _require(isinstance(frozen, dict) and set(frozen) == set(CORE_READER_FILES)
             and all(isinstance(value, str) and re.fullmatch(r"[0-9a-f]{64}", value)
                     for value in (*frozen.values(), *current.values())), "candidate freeze is incomplete")
    if frozen == current:
        return None
    return {"kind": "complete_exact_reader_reread", "reference_sha256": _sha(path),
            "historical_frozen_reader_files": frozen, "current_reader_files": current,
            "complete_source_set_reread": True,
            "held_out_provenance": "Original frozen reader only; current reader reuses independent labels."}


def _label(label):
    """Validate the independently observed full field and its per-slot components."""
    _require(label.get("field") == "secondary" and label.get("state") in
             {"readable", "ambiguous", "unreadable", "unresolved"}
             and isinstance(label.get("reason"), str) and label["reason"], "literal label is malformed")
    cards = deepcopy(label.get("cards"))
    _require(isinstance(cards, list) and len(cards) == 2
             and all(isinstance(card, dict) for card in cards),
             "literal label must retain both card slots")
    for card in cards:
        if card.get("slot") in ("left", "right"):
            card["slot"] = {"left": 0, "right": 1}[card["slot"]]
    _require(all(type(card.get("slot")) is int for card in cards)
             and {card["slot"] for card in cards} == {0, 1},
             "literal label must retain both card slots")
    complete, values, by_slot = True, [], {}
    for card in sorted(cards, key=lambda card: card["slot"]):
        _require(type(card["slot"]) is int and card.get("presence") in
                 {"present", "absent", "uncertain"}, "literal slot presence is malformed")
        by_slot[card["slot"]] = card
        components = {key: card.get(key) for key in ("band", "frequency", "direction", "bars")}
        _require(components["band"] is None or components["band"] in {"X", "K", "Ka", "Ku", "L"},
                 "literal band is malformed")
        _require(components["frequency"] is None or isinstance(components["frequency"], str)
                 and re.fullmatch(r"[0-9]{1,2}\.[0-9]{3}", components["frequency"]),
                 "literal frequency is malformed")
        _require(components["direction"] is None or components["direction"] in {"front", "side", "rear"},
                 "literal direction is malformed")
        _require(components["bars"] is None or type(components["bars"]) is int
                 and 0 <= components["bars"] <= 6, "literal bars are malformed")
        if card["presence"] == "absent":
            _require(all(value is None for value in components.values()), "absent slot contains asserted content")
        elif card["presence"] == "present":
            complete = complete and all(value is not None for value in components.values())
            values.append(components)
        else:
            complete = False
    if label["state"] == "readable":
        _require(complete and label.get("value") == values, "readable field differs from literal slots")
    else:
        _require(label.get("value") is None, "unresolved label asserts a full field")
    return {"state": label["state"], "value": label.get("value")}, by_slot


def validate_reference(path, method, observe, *, camera=None, reader_reanalysis=None):
    """Reread every selected secondary original and verify every new assertion."""
    from PIL import Image
    import numpy as np
    from encounter_qualification import _derived_field_status, _partial_secondary_cards

    path = Path(path).resolve()
    document, manifest_path, packet = _packet(path)
    _require(reference_reread_binding(path, method) == reader_reanalysis,
             "reader differs from immutable candidate freeze or declared reread")
    for name in ("protocol", "selection_before_reading"):
        _artifact(path.parent, document.get(name))
    for ref in document.get("provenance_artifacts", []):
        _artifact(path.parent, ref)
    labels_document = _read(_artifact(path.parent, document.get("observations")))
    _require(labels_document.get("schema_version") == 1
             and labels_document.get("blind_protocol") == BLIND_PROTOCOL,
             "independent observation provenance is incomplete")
    all_labels = _indexed(labels_document.get("items"), "blind labels")
    labels = {key: value for key, value in all_labels.items() if value.get("field") == "secondary"}
    items = _indexed(document.get("items"), "supplement")
    _require(items and set(items) == set(packet) == set(labels), "complete selected secondary subset differs")
    counts, roles, split_roles, band_pixel_roles = Counter(), Counter(), Counter(), Counter()
    hashes, sources, calibrations = set(), set(), {}
    partial_assertions = split_agreements = band_pixel_agreements = 0
    for opaque_id, item in items.items():
        image = _artifact(path.parent, item.get("image"))
        digest = _sha(image)
        _require(digest not in hashes, "original image is duplicated")
        hashes.add(digest)
        record, label = packet[opaque_id], labels[opaque_id]
        crop = _artifact(manifest_path.parent, {"path": record.get("image"), "sha256": record.get("sha256")})
        _require(label.get("image_sha256") == record.get("sha256"), "label differs from its opaque crop")
        box = item.get("observer_crop_box")
        _require(isinstance(box, list) and len(box) == 4 and all(type(value) is int for value in box),
                 "observer crop mapping is missing")
        with Image.open(image) as full, Image.open(crop) as viewed:
            _require(0 <= box[0] < box[2] <= full.width and 0 <= box[1] < box[3] <= full.height
                     and np.array_equal(np.asarray(full.convert("RGB").crop(box)),
                                        np.asarray(viewed.convert("RGB"))),
                     "opaque observer crop differs from original pixels")
        reference, literal_slots = _label(label)
        origin = item.get("origin")
        _require(isinstance(origin, dict) and origin.get("role") in ROLES
                 and isinstance(origin.get("capture_id"), str) and bool(origin["capture_id"])
                 and isinstance(origin.get("video_sha256"), str)
                 and re.fullmatch(r"[0-9a-f]{64}", origin["video_sha256"])
                 and type(origin.get("video_frame_index")) is int and origin["video_frame_index"] >= 0,
                 "original source identity is incomplete")
        roles[origin["role"]] += 1
        sources.add((origin["capture_id"], origin["video_sha256"]))
        _require(isinstance(item.get("startup_calibration"), dict), "original startup inputs are missing")
        registration = _item_registration(item, item.get("registration"), path.parent, calibrations)
        preflight = _read(_artifact(path.parent, item["startup_calibration"]["preflight"]))
        if camera is not None:
            actual = preflight.get("camera", {})
            _require({"name": actual.get("name"), "profile": actual.get("profile")} == camera,
                     "original camera profile differs from qualification")
        observed = observe(image, registration).get("secondary")
        status = _derived_field_status("secondary", observed, reference)
        _require(status not in {"WRONG_ASSERTION", "ASSERTION_WITHOUT_RESOLVED_REFERENCE"},
                 "reader contradicts the independent label: " + opaque_id)
        if observed.get("state") == "readable":
            _require(observed.get("value") == reference["value"], "reader changed literal card association: " + opaque_id)
        partial = _partial_secondary_cards(observed) or []
        observed_cards = observed.get("cards", [])
        if partial:
            _require(isinstance(observed_cards, list) and len(observed_cards) == len(partial),
                     "partial identities have no card-slot association")
            for card, value in zip(observed_cards, partial):
                if value["band"] is not None and value["frequency"] is not None:
                    literal = literal_slots.get(card.get("slot"), {})
                    _require(literal.get("presence") == "present"
                             and (value["band"], value["frequency"]) ==
                             (literal.get("band"), literal.get("frequency")),
                             "unsupported partial card identity: " + opaque_id)
                    partial_assertions += 1
        for card in observed_cards:
            split = card.get("split_text_observation", {})
            if split.get("accepted") is True and card.get("band") is not None and card.get("frequency") is not None:
                literal = literal_slots.get(card.get("slot"), {})
                _require(literal.get("presence") == "present"
                         and (card["band"], card["frequency"]) == (literal.get("band"), literal.get("frequency")),
                         "unsupported split-text card identity: " + opaque_id)
                split_agreements += 1
                split_roles[origin["role"]] += 1
            band_pixel = card.get("band_pixel_observation", {})
            if (band_pixel.get("method") == "complete_card_band/v1"
                    and band_pixel.get("accepted") is True
                    and card.get("band") is not None and card.get("frequency") is not None):
                literal = literal_slots.get(card.get("slot"), {})
                _require(literal.get("presence") == "present"
                         and (card["band"], card["frequency"]) == (literal.get("band"), literal.get("frequency")),
                         "unsupported complete-band card identity: " + opaque_id)
                band_pixel_agreements += 1
                band_pixel_roles[origin["role"]] += 1
        counts[status] += 1
    return {"unique_original_frames": len(items), "counts": dict(counts), "roles": dict(roles),
            "source_recordings": len(sources), "partial_identity_assertions": partial_assertions,
            "split_text_agreements": split_agreements, "split_text_agreements_by_role": dict(split_roles),
            "band_pixel_agreements": band_pixel_agreements,
            "band_pixel_agreements_by_role": dict(band_pixel_roles),
            "reader_reanalysis": reader_reanalysis is not None}
