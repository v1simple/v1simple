"""Count/mode candidates from recorded ESP input, independent of image readers.

Wire references: Valentine Research iOS ESP Library, ESPPacket.m (framing),
ESPAlertData.m (row count/index and fields), ESPDisplayData.m (segment planes).
The seven segment bits are top, upper-right, lower-right, bottom, lower-left,
upper-left, middle. The two images alternate in ONE physical glyph position.

This is sampled agreement with host-accepted input, not DUT receipt or a response
deadline. The caller binds artifacts, normal runtime, configuration and camera
timestamps. No renderer, emulator encoder, or stimulus.expected values are used.
"""

from __future__ import annotations

import hashlib
import math
import re
from typing import Any


class CounterEvidenceError(ValueError):
    """Recorded inputs cannot support a coherent sampled comparison."""


_DIGITS = dict(zip((0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F), range(10)))
_MODES = {0x77: "A", 0x38: "L", 0x18: "l"}
_BANDS = {"laser": 1, "ka": 2, "k": 4, "x": 8, "ku": 16}
_DIRECTIONS = {"FRONT": 0x20, "SIDE": 0x40, "REAR": 0x80}


def _require(condition: bool, reason: str) -> None:
    if not condition:
        raise CounterEvidenceError(reason)


def _integer(value: Any, name: str, minimum: int = 0) -> int:
    _require(type(value) is int and value >= minimum, f"invalid {name}")
    return value


def _packet(hex_text: Any) -> tuple[bytes, int, bytes]:
    _require(isinstance(hex_text, str) and re.fullmatch(r"(?:[0-9a-fA-F]{2})+", hex_text) is not None,
             "invalid packet hex")
    raw = bytes.fromhex(hex_text)
    _require(len(raw) >= 7 and raw[0] == 0xAA and raw[-1] == 0xAB, "invalid ESP framing")
    _require(raw[1] & 0xF0 == 0xD0 and raw[2] & 0xF0 == 0xE0, "invalid ESP address prefix")
    _require(raw[4] >= 1 and len(raw) == raw[4] + 6, "invalid ESP length")
    _require(sum(raw[:-2]) & 0xFF == raw[-2], "invalid ESP checksum")
    return raw, raw[3], raw[5:-2]


def _unknown(reason: str) -> dict[str, dict]:
    return {name: {"unresolved": reason} for name in ("count", "mode")}


def decode_counter_packet(payload_hex: str) -> dict:
    """Validate a framed infDisplayData packet and return count/mode alternatives.

    Decimal points are outside this scope. Unknown glyphs leave BOTH fields
    unresolved; a canonical blank plane permits absence during a blink phase.
    """
    raw, packet_id, data = _packet(payload_hex)
    _require(packet_id == 0x31 and len(data) == 8 and raw[2] == 0xEA,
             "expected V1 infDisplayData with eight payload bytes")
    masks = list(dict.fromkeys(byte & 0x7F for byte in data[:2]))
    fields: dict = {"count": {"allowed": []}, "mode": {"allowed": []}}
    pairs = []
    for mask in masks:
        if mask in _DIGITS:
            count, mode = _DIGITS[mask], None
        elif mask in _MODES:
            count, mode = None, _MODES[mask]
        elif mask == 0:
            count, mode = None, None
        else:
            return {"fields": _unknown("counter segment pattern is outside digits/A/L/l/blank"),
                    "segment_masks": masks, "permitted_pairs": []}
        pairs.append({"count": count, "mode": mode})
        for name, value in (("count", count), ("mode", mode)):
            if value not in fields[name]["allowed"]:
                fields[name]["allowed"].append(value)
    if any(pair["count"] is not None for pair in pairs) and any(pair["mode"] is not None for pair in pairs):
        fields = _unknown("mixed count/mode planes require a joint glyph comparison")
    return {"fields": fields, "segment_masks": masks, "permitted_pairs": pairs}


def _scenario_rows(sample: dict, notifications: list[dict]) -> int:
    alerts = sample.get("alerts")
    _require(isinstance(alerts, list) and len(alerts) <= 15, "invalid scenario alert list")
    rows = []
    displays = []
    for notification in notifications:
        _require(isinstance(notification, dict), "invalid planned notification")
        raw, packet_id, data = _packet(notification.get("bytesHex"))
        _require(raw[2] == 0xEA, "planned packet is not from V1")
        if packet_id == 0x43 and notification.get("kind") == "alert_row":
            _require(len(data) == 7, "invalid alert-row length")
            rows.append(data)
        elif packet_id == 0x31 and notification.get("kind") == "display_frame":
            displays.append(decode_counter_packet(notification["bytesHex"]))
        else:
            raise CounterEvidenceError("unsupported planned notification kind or packet")
    _require(len(displays) == 1 and len(rows) == max(1, len(alerts)),
             "scenario requires one display and a complete alert table")
    count = len(alerts)
    _require([row[0] for row in rows] == ([0] if count == 0 else [index * 16 + count for index in range(1, count + 1)]),
             "wire alert indices/count disagree with scenario")
    if not alerts:
        _require(rows == [bytes(7)], "idle alert table must be empty")
    for alert, row in zip(alerts, rows):
        _require(isinstance(alert, dict), "invalid scenario alert")
        band = _BANDS.get(str(alert.get("band")).lower())
        direction = _DIRECTIONS.get(str(alert.get("direction")))
        frequency = _integer(alert.get("frequencyMHz"), "scenario frequency")
        _require(band is not None and direction is not None and frequency <= 65535
                 and type(alert.get("priority")) is bool, "unsupported scenario alert semantics")
        _require((row[5] & 0x1F, row[5] & 0xE0, int.from_bytes(row[1:3], "big"), bool(row[6] & 0x80))
                 == (band, direction, frequency, alert["priority"]), "wire alert semantics disagree with scenario")
        _require(("bandMask" not in alert or alert["bandMask"] == band)
                 and ("directionMask" not in alert or alert["directionMask"] == direction),
                 "scenario alert mask disagrees with its named semantics")
    # Numeric glyphs in this simple replay scope must reflect the table count.
    # Verdict/volume glyphs may exist in ESP but are not inferred as alert counts.
    pairs = displays[0]["permitted_pairs"]
    _require(all(pair["count"] in (None, count) for pair in pairs),
             "wire counter digit disagrees with scenario alert count")
    return count


def build_counter_timeline(scenario: dict, stimulus_records: list[dict],
                           delivery_records: list[dict]) -> dict:
    """Cross-check scenario semantics and every planned/requested/accepted packet.

    Malformed, missing, duplicate or terminally lost delivery is an evidence
    error. Backpressure retries are permitted. Unscoped accepted display packets
    remain in the timeline; an unscoped alert table makes its context unresolved.
    """
    _require(isinstance(scenario, dict) and type(scenario.get("schemaVersion")) is int
             and scenario["schemaVersion"] == 1,
             "unsupported scenario schema")
    samples = scenario.get("samples")
    _require(isinstance(samples, list) and bool(samples)
             and isinstance(stimulus_records, list) and bool(stimulus_records)
             and isinstance(delivery_records, list) and bool(delivery_records),
             "empty scenario, stimulus or delivery evidence")
    samples_by_index = {}
    for sample in samples:
        _require(isinstance(sample, dict), "invalid scenario sample")
        index = _integer(sample.get("sourceIndex"), "scenario source index")
        _require(index not in samples_by_index, "duplicate scenario source index")
        samples_by_index[index] = sample
    planned = {}
    stimuli = []
    for record in stimulus_records:
        _require(isinstance(record, dict) and type(record.get("schemaVersion")) is int and record["schemaVersion"] == 2
                 and record.get("state") == "stimulus_requested", "unsupported stimulus record")
        sequence = _integer(record.get("stimulusSequence"), "stimulus sequence", 1)
        index = _integer(record.get("sourceIndex"), "stimulus source index")
        requested_ns = _integer(record.get("requestedHostMonotonicNs"), "stimulus request timestamp")
        offset = record.get("replayOffsetSeconds")
        _require(type(offset) in (float, int) and math.isfinite(offset) and offset >= 0,
                 "invalid stimulus offset")
        sample = samples_by_index.get(index)
        _require(sample is not None and sample.get("offsetSeconds") == offset,
                 "scenario source index/offset differs from stimulus")
        _require(not stimuli or (sequence > stimuli[-1]["stimulus_sequence"]
                 and requested_ns > stimuli[-1]["stimulus_requested_ns"]
                 and offset >= stimuli[-1]["offset_seconds"]), "nonmonotonic stimulus identity/time")
        notifications = record.get("notifications")
        _require(isinstance(notifications, list) and notifications, "missing planned notifications")
        count = _scenario_rows(sample, notifications)
        for ordinal, notification in enumerate(notifications):
            _require(type(notification.get("ordinal")) is int and notification["ordinal"] == ordinal
                     and notification.get("channel") == "display_short",
                     "invalid planned ordinal/channel")
            planned[(sequence, ordinal)] = bytes.fromhex(notification["bytesHex"])
        stimuli.append({"stimulus_sequence": sequence, "source_index": index,
                        "stimulus_requested_ns": requested_ns, "offset_seconds": offset,
                        "alert_count": count, "planned_count": len(notifications)})
    _require(len(stimuli) == len(samples) and len({item["source_index"] for item in stimuli}) == len(samples),
             "scenario and stimulus sample sets differ")

    tracks: dict[int, dict] = {}
    scoped = {}
    accepted = []
    for record in delivery_records:
        _require(isinstance(record, dict) and type(record.get("schemaVersion")) is int and record["schemaVersion"] == 3,
                 "unsupported delivery schema")
        tx = _integer(record.get("globalTxSequence"), "global transmission sequence", 1)
        now = _integer(record.get("hostMonotonicNs"), "delivery timestamp")
        raw, packet_id, _ = _packet(record.get("payloadHex"))
        _require(record.get("payloadSha256") == hashlib.sha256(raw).hexdigest(), "delivery payload hash mismatch")
        _require(record.get("characteristic") == "B2CE", "unsupported delivery characteristic")
        sequence, ordinal = record.get("stimulusSequence"), record.get("emissionOrdinal")
        key = None
        if sequence is not None or ordinal is not None:
            key = (_integer(sequence, "delivery stimulus sequence", 1), _integer(ordinal, "delivery ordinal"))
            _require(key in planned and planned[key] == raw, "planned/delivered packet mismatch")
        identity = (raw, key, record["characteristic"])
        state = record.get("state")
        if state == "notification_requested":
            _require(tx not in tracks and (key is None or key not in scoped), "duplicate requested delivery")
            tracks[tx] = {"identity": identity, "requested_ns": now, "last_ns": now, "accepted_ns": None}
            if key is not None:
                scoped[key] = tx
        elif state in ("notification_delayed", "notification_accepted"):
            track = tracks.get(tx)
            _require(track is not None and track["accepted_ns"] is None and track["identity"] == identity,
                     "orphan, duplicate or changed delivery outcome")
            attempted = _integer(record.get("attemptedHostMonotonicNs"), "delivery attempt timestamp")
            _require(track["last_ns"] <= attempted <= now, "nonmonotonic delivery attempt")
            track["last_ns"] = now
            if state == "notification_accepted":
                track["accepted_ns"] = now
                accepted.append({"global_tx_sequence": tx, "packet_id": packet_id,
                                 "payload_hex": raw.hex(), "stimulus_sequence": sequence,
                                 "display_requested_ns": track["requested_ns"], "display_accepted_ns": now})
        else:
            raise CounterEvidenceError("terminal loss or unsupported delivery state")
    _require(all(track["accepted_ns"] is not None for track in tracks.values())
             and set(scoped) == set(planned), "missing accepted/planned delivery")
    for stimulus in stimuli:
        times = [tracks[scoped[(stimulus["stimulus_sequence"], ordinal)]]
                 for ordinal in range(stimulus.pop("planned_count"))]
        _require(all(track["requested_ns"] >= stimulus["stimulus_requested_ns"] for track in times),
                 "notification precedes its stimulus request")
        _require([track["accepted_ns"] for track in times] == sorted(track["accepted_ns"] for track in times),
                 "planned notification acceptance order changed")
        stimulus["all_accepted_ns"] = max(track["accepted_ns"] for track in times)
    return {"stimuli": stimuli, "accepted": sorted(accepted, key=lambda item: (item["display_accepted_ns"], item["global_tx_sequence"]))}


def counter_expectation_at(timeline: dict, capture_ns: int, *,
                           stealth_enabled: bool | None = None) -> dict:
    """Return sampled input candidates and their basis; impose no response deadline.

    Normal live, persisted and idle paths use CURRENT V1 glyph planes
    (render_frame_composer.cpp and display_update.cpp). Alert persistence does
    not retain an old counter. Idle stealth can replace this slot entirely.
    """
    _integer(capture_ns, "capture timestamp")
    _require(stealth_enabled is None or type(stealth_enabled) is bool, "invalid stealth precondition")
    result: dict = {"fields": _unknown("no validated stimulus at capture time"), "input": {},
                    "comparison_basis": ["Sampled agreement with recorded input; no response deadline.",
                                         "CoreBluetooth acceptance does not establish DUT receipt."]}
    candidates = [item for item in timeline["stimuli"] if item["stimulus_requested_ns"] <= capture_ns]
    if not candidates:
        return result
    stimulus = candidates[-1]
    result["input"].update(stimulus)
    if any(item["packet_id"] == 0x31 and item["display_requested_ns"] <= capture_ns < item["display_accepted_ns"]
           for item in timeline["accepted"]):
        result["fields"] = _unknown("display transmission is pending at capture time")
        return result
    if stimulus["all_accepted_ns"] > capture_ns:
        result["fields"] = _unknown("current stimulus notifications were not all accepted by capture time")
        return result
    events = [item for item in timeline["accepted"] if item["display_accepted_ns"] <= capture_ns]
    displays = [item for item in events if item["packet_id"] == 0x31]
    if not displays:
        return result
    display = displays[-1]
    result["input"].update({key: value for key, value in display.items() if key != "stimulus_sequence"})
    result["input"]["display_stimulus_sequence"] = display["stimulus_sequence"]
    if any(item["packet_id"] == 0x43 and item["stimulus_sequence"] is None
           and item["display_accepted_ns"] >= stimulus["all_accepted_ns"] for item in events):
        result["fields"] = _unknown("unscoped alert table superseded validated scenario context")
        return result
    if display["stimulus_sequence"] not in (None, stimulus["stimulus_sequence"]):
        result["fields"] = _unknown("accepted display belongs to a different stimulus")
        return result
    decoded = decode_counter_packet(display["payload_hex"])
    result["permitted_pairs"] = decoded["permitted_pairs"]
    if any(pair["count"] not in (None, stimulus["alert_count"]) for pair in decoded["permitted_pairs"]):
        result["fields"] = _unknown("unscoped counter digit disagrees with current scenario context")
        return result
    if stimulus["alert_count"] and any(pair["mode"] is not None for pair in decoded["permitted_pairs"]):
        result["fields"] = _unknown("mode glyph during declared live alerts is outside the simple count replay rule")
        return result
    if stimulus["alert_count"] == 0 and stealth_enabled is not False:
        result["fields"] = _unknown("idle counter requires independently established stealthEnabled=false")
        return result
    result["fields"] = decoded["fields"]
    result["comparison_basis"].extend([
        "Scenario row count, indices, band, direction, frequency and priority agree with planned and accepted wire bytes.",
        "Count/mode decode the latest accepted display's one glyph slot; differing planes permit either phase.",
        "Current counter state is used across normal live, persisted and idle rendering; no persistence delay is assumed.",
        "Decimal point, other display fields, physical receipt and temporal correctness are outside this scope.",
    ])
    return result
