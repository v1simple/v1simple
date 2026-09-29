#!/usr/bin/env python3
"""Prepare a private, temporary USB bundle for the profile-controls bench.

This performs no device I/O. Keep the input backup for catalog/slot restoration.
Restoring that bundle does not restore detector sweep ranges or the per-detector
applied In-the-Box snapshot; those require an explicit restoration Apply.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import usb_profiles as usb


PROFILE_NAME = "Bench Profile Controls"


def parse_user_bytes(value):
    usb.require(type(value) is str and re.fullmatch(r"[0-9a-fA-F]{12}", value) is not None,
                "Baseline user bytes must be exactly twelve hexadecimal digits")
    return list(bytes.fromhex(value))


def prepare_bundle(original, baseline_user_bytes, *, inactive_box_policy=False):
    """Preserve the normalized catalog and unrelated slot state in a new copy."""
    usb.require(type(baseline_user_bytes) is list and len(baseline_user_bytes) == 6
                and all(usb.integer(value, 0, 255) for value in baseline_user_bytes),
                "Baseline user bytes must contain exactly six bytes")
    bundle = usb.migrate_bundle(original)
    usb.require(all(profile["name"].translate(usb.ASCII_LOWER) != PROFILE_NAME.lower()
                    for profile in bundle["profiles"]),
                "Reserved bench profile already exists; no profile was replaced")
    usb.require(len(bundle["profiles"]) < usb.MAX_PROFILE_COUNT,
                "The profile catalog needs one free slot for the bench profile")

    raw = list(baseline_user_bytes)
    # ESP user bytes: K, Ka and laser use byte 0 bits 1..3; Custom Frequencies
    # uses the inverted byte 1 bit 3. Preserve every other baseline bit.
    raw[0] |= 0x0E
    raw[1] &= ~0x08
    detector = usb.default_detector()
    detector["customFrequencies"] = {"policy": "value", "definitions": [
        {"index": 0, "lowerMHz": 24000, "upperMHz": 24200},
        {"index": 1, "lowerMHz": 33900, "upperMHz": 35000},
    ]}
    in_the_box = usb.default_in_the_box()
    if not inactive_box_policy:
        in_the_box["bands"]["k"] = {"muteOutside": True, "unmuteInside": True}
        in_the_box["boxes"]["k"]["upperMHz"] = 24150
    box_description = "inactive default boxes" if inactive_box_policy else "edited K box"
    bundle["profiles"].append({
        "schemaVersion": 4, "name": PROFILE_NAME,
        "description": f"Temporary emulator bench profile: custom scan ranges and {box_description}.",
        "rawBytes": raw, "detector": detector, "inTheBox": in_the_box,
    })
    bundle["autoPushEnabled"] = True
    for slot in bundle["slots"]:
        slot.update(usb.SLOT_MODIFIER_DEFAULTS)
        slot.update(profile=PROFILE_NAME, alertPersist=0, priorityArrowOnly=False)
    # Reuse the shipping client's complete shape and encoded-size checks.
    usb.encode_bundle(bundle)
    return bundle


def prepare_file(source, destination, baseline_user_bytes, *, inactive_box_policy=False):
    source, destination = Path(source), Path(destination)
    usb.require(source.resolve() != destination.resolve(), "Output must differ from the input backup")
    raw = source.read_bytes()
    usb.require(0 < len(raw) <= usb.MAX_PAYLOAD, "Input backup exceeds the supported payload size")
    prepared = prepare_bundle(usb.parse_json(raw), baseline_user_bytes,
                              inactive_box_policy=inactive_box_policy)
    usb.save_private(destination, usb.encode_bundle(prepared))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("backup", type=Path, help="original USB profile backup, kept unchanged")
    parser.add_argument("output", type=Path, help="new private bundle; must not already exist")
    parser.add_argument("--baseline-user-bytes", required=True, metavar="HEX12",
                        help="fresh detector user bytes or the isolated emulator's known baseline")
    parser.add_argument("--inactive-box-policy", action="store_true",
                        help="use exact default boxes with all actions off for a negative control "
                             "or restoration of a known default applied policy; scan ranges stay unchanged")
    args = parser.parse_args(argv)
    try:
        prepare_file(args.backup, args.output, parse_user_bytes(args.baseline_user_bytes),
                     inactive_box_policy=args.inactive_box_policy)
    except (usb.ProfileError, OSError) as exc:
        message = str(exc) if isinstance(exc, usb.ProfileError) else "Could not read the backup or create a new output file"
        parser.exit(1, f"Profile-controls preparation failed: {message}\n")
    print("Temporary profile-controls bundle created. Original backup unchanged; no device was contacted.")
    print("Restore detector ranges and the applied In-the-Box policy before restoring the original bundle.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
