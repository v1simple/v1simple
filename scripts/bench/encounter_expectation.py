"""Sampled radar encounter expectations from validated wire evidence, never pixels.

Host acceptance is not DUT receipt. Comparisons describe sampled input agreement;
transition probes do not impose an invented firmware response deadline. The caller
must bind any supplied configuration to this recording's runtime and capture range.
"""
from __future__ import annotations

from collections import Counter
from copy import deepcopy
import re

try:
    from .counter_expectation import (CounterEvidenceError, _packet,
                                     build_counter_timeline, counter_expectation_at)
except ImportError:
    from counter_expectation import (CounterEvidenceError, _packet,
                                     build_counter_timeline, counter_expectation_at)


class EncounterEvidenceError(CounterEvidenceError):
    """The recorded inputs are malformed or outside this instrument's scope."""


FIELDS = ("counter_glyph", "primary_frequency", "active_bands", "main_arrows",
          "main_bars", "secondary", "muted_badge")
_BANDS = {2: "Ka", 4: "K", 8: "X"}
_DIRECTIONS = {32: "front", 64: "side", 128: "rear"}
_LED_BARS = {0: 0, 1: 1, 3: 2, 7: 3, 15: 4, 31: 5, 63: 6, 127: 7, 255: 8}
_GLYPHS = dict(zip((63, 6, 91, 79, 102, 109, 125, 7, 127, 111), "0123456789"))
_GLYPHS.update({119: "A", 56: "L", 24: "l", 0: None})
# Valentine Research ESPAlertData's per-band RSSI thresholds. Main display bars
# use the separate display LED bitmap; secondary cards use the six-cell mapping
# in include/display_visual_contract.h, not that main-bar clamp.
_RSSI = {"Ka": (1, 144, 151, 158, 165, 172, 179, 186),
         "K": (1, 136, 144, 154, 164, 174, 184, 194),
         "X": (1, 150, 160, 170, 180, 189, 197, 208)}
_CARD_BARS = (0, 1, 2, 2, 3, 4, 5, 5, 6)


def _require(condition, reason):
    if not condition:
        raise EncounterEvidenceError(reason)


def _unique(values):
    return [value for index, value in enumerate(values) if value not in values[:index]]


def _row(hex_text):
    _, packet_id, row = _packet(hex_text)
    _require(packet_id == 0x43 and len(row) == 7, "invalid encounter alert row")
    if row == bytes(7):
        return None
    band, direction = _BANDS.get(row[5] & 31), _DIRECTIONS.get(row[5] & 224)
    _require(band is not None and direction is not None,
             "encounter scope supports ordinary X/K/Ka radar directions only")
    _require(row[6] & 127 == 0, "junk/photo/reserved alert flags are outside encounter scope")
    frequency = int.from_bytes(row[1:3], "big")
    _require(frequency > 0, "zero-frequency radar is not a renderable ordinary encounter alert")
    raw = row[3] if direction == "front" else row[4] if direction == "rear" else max(row[3:5])
    bars = sum(raw >= threshold for threshold in _RSSI[band])
    return {"band": band, "frequency": f"{frequency // 1000}.{frequency % 1000:03}",
            "direction": direction, "bars": _CARD_BARS[bars], "vr_bars": bars,
            "priority": bool(row[6] & 128), "row_index": row[0] >> 4}


def _display(hex_text):
    _, packet_id, data = _packet(hex_text)
    _require(packet_id == 0x31 and len(data) == 8, "invalid encounter display packet")
    _require(data[2] in _LED_BARS, "unsupported main LED bitmap")
    _require(not data[4] & ~data[3], "inverse band/arrow blink planes are outside encounter scope")
    _require(not (data[3] | data[4]) & 1, "display-only laser is outside encounter scope")
    _require(all(mask & 127 in _GLYPHS for mask in data[:2]), "unsupported counter glyph")
    searching = bool(data[5] & 4)
    phases = []
    # display.h and drawStatusStrip in display_update.cpp use the SAME
    # blinkPhase_ for this glyph, bands and arrows. Do not cross-product them.
    for glyph, mask in zip(data[:2], data[3:5]):
        phases.append({"counter_glyph": _GLYPHS[glyph & 127],
                       "active_bands": [label for bit, label in _BANDS.items() if searching and mask & bit],
                       "main_arrows": [label for bit, label in _DIRECTIONS.items() if searching and mask & bit]})
    return {"phases": phases, "main_bars": min(6, _LED_BARS[data[2]]),
            "mute_bit": bool(data[3] & 16), "soft_muted": bool(data[5] & 1),
            "system_status": searching, "main_volume": data[7] >> 4,
            "image1": data[3], "image2": data[4]}


def _card(row):
    return {key: row[key] for key in ("band", "frequency", "direction", "bars")}


def _identity(card):
    return card["band"], card["frequency"]


def build_encounter_timeline(scenario, stimulus, delivery):
    """Validate all planned/requested/accepted input before producing any verdict."""
    try:
        timeline = build_counter_timeline(scenario, stimulus, delivery)
        authored = {sample["sourceIndex"]: sample for sample in scenario["samples"]}
        states = []
        for source, timing in zip(stimulus, timeline["stimuli"]):
            notifications = source["notifications"]
            _require([n["kind"] for n in notifications] ==
                     ["alert_row"] * (len(notifications) - 1) + ["display_frame"],
                     "encounter requires complete table followed by display")
            rows = [_row(n["bytesHex"]) for n in notifications[:-1]]
            rows = [row for row in rows if row is not None]
            _require(len(rows) <= 3, "encounter scope supports at most two secondary alert cards")
            _require(sum(row["priority"] for row in rows) == bool(rows),
                     "live encounter requires exactly one priority alert")
            _require(len({_identity(row) for row in rows}) == len(rows),
                     "duplicate band/frequency alert identities are outside encounter scope")
            _require(not any(left["band"] == right["band"] and
                             abs(int(left["frequency"].replace(".", "")) -
                                 int(right["frequency"].replace(".", ""))) <= 5
                             for index, left in enumerate(rows) for right in rows[index + 1:]),
                     "same-band rows within 5 MHz require card identity/continuity coalescing; "
                     "outside distinct-alert encounter scope")
            display = _display(notifications[-1]["bytesHex"])
            sample = authored[timing["source_index"]]
            for row, alert in zip(rows, sample["alerts"]):
                if "strength" in alert:
                    _require(type(alert["strength"]) is int and row["vr_bars"] == alert["strength"],
                             "wire RSSI disagrees with authored alert strength")
            if "muted" in sample:
                _require(type(sample["muted"]) is bool and display["soft_muted"] == sample["muted"],
                         "wire soft-mute state disagrees with authored input")
            states.append({**timing, "rows": rows, "display": display,
                           "packet_signature": [n["bytesHex"].lower() for n in notifications]})
        muted_run = 0
        for event in timeline["accepted"]:
            if event["packet_id"] in (0x02, 0x3D):
                # Replay's version/volume handshake precedes the encounter.
                # These packets do not encode any of the seven checked fields.
                _, packet_id, payload = _packet(event["payload_hex"])
                valid = (bool(re.fullmatch(rb"[vV][0-9]\.[0-9]{4}", payload)) if packet_id == 0x02
                         else len(payload) == 4 and all(value <= 9 for value in payload))
                _require(valid and event["stimulus_sequence"] is None and
                         event["display_accepted_ns"] < states[0]["stimulus_requested_ns"],
                         "unsupported encounter version/volume handshake")
                continue
            _require(event["packet_id"] in (0x31, 0x43), "unsupported accepted encounter packet")
            if event["packet_id"] == 0x31:
                event["decoded_display"] = _display(event["payload_hex"])
                muted_run = muted_run + 1 if event["decoded_display"]["mute_bit"] else 0
                event["consecutive_muted_displays"] = muted_run
            else:
                _row(event["payload_hex"])
        timeline["states"] = states
        return timeline
    except CounterEvidenceError as error:
        raise EncounterEvidenceError(str(error)) from error


def _configuration(configuration):
    if configuration is None:
        return {}
    _require(isinstance(configuration, dict), "invalid encounter configuration")
    _require(set(configuration) <= {"stealthEnabled", "priorityArrowOnly", "alertPersistenceSeconds"},
             "unsupported encounter configuration field")
    for key in ("stealthEnabled", "priorityArrowOnly"):
        _require(key not in configuration or type(configuration[key]) is bool, f"invalid {key}")
    if "alertPersistenceSeconds" in configuration:
        value = configuration["alertPersistenceSeconds"]
        _require(type(value) is int and 0 <= value <= 5, "invalid alertPersistenceSeconds")
    return configuration


def _presentation(rows, display, muted, configuration, historical):
    primary = next((row for row in rows if row["priority"]), None)
    live = primary is not None
    fields = {name: {"unresolved": "idle presentation requires boot-bound stealth/persistence settings"}
              for name in FIELDS}
    fields.update(main_bars={"allowed": [display["main_bars"] if live else 0]},
                  muted_badge={"allowed": [muted if live else False]},
                  secondary={"allowed": [[_card(row) for row in rows if not row["priority"]]]})
    phases = deepcopy(display["phases"])
    if live:
        fields["primary_frequency"] = {"allowed": [primary["frequency"]]}
        if configuration.get("priorityArrowOnly") is True:
            for phase in phases:
                phase["main_arrows"] = [x for x in phase["main_arrows"] if x == primary["direction"]]
        elif "priorityArrowOnly" not in configuration and any(
                any(arrow != primary["direction"] for arrow in phase["main_arrows"]) for phase in phases):
            for phase in phases:
                phase.pop("main_arrows")
            fields["main_arrows"] = {"unresolved": "priorityArrowOnly changes the applicable arrow set"}
        for name in ("counter_glyph", "active_bands", "main_arrows"):
            if name in phases[0]:
                fields[name] = {"allowed": _unique([phase[name] for phase in phases])}
    else:
        # Resting/persisted modes clear cards and mute. Resting draws the
        # current LED bitmap and image1 bands; persisted draws zero bars and
        # its prior primary band. Host time cannot establish that expiry.
        if display["main_bars"]:
            fields["main_bars"] = {
                "unresolved": "nonzero idle LED bitmap differs between resting and persisted/stealth presentation"}
        if configuration.get("stealthEnabled") is False:
            fields["counter_glyph"] = {"allowed": _unique([phase["counter_glyph"] for phase in phases])}
            if configuration.get("alertPersistenceSeconds") == 0:
                # update(DisplayState) draws the dash frequency whenever the
                # accepted volume is nonzero. At zero, its warning state machine
                # can replace this region; proxy/speed-mute context and warning
                # phase are not established by these seven-field inputs.
                fields["primary_frequency"] = (
                    {"allowed": ["--.---"]} if display["main_volume"] > 0 else
                    {"unresolved": "zero-volume warning can replace the idle frequency; "
                                   "its runtime context and phase are unestablished"})
                fields["main_bars"] = {"allowed": [display["main_bars"]]}
                fields["active_bands"] = {"allowed": [display["phases"][0]["active_bands"]]}
                fields["main_arrows"] = {"allowed": [[]]}
        phases = [{key: value for key, value in phase.items()
                   if "allowed" in fields[key]} for phase in phases]
        if "allowed" in fields["active_bands"]:
            phases = [{**phase, "active_bands": fields["active_bands"]["allowed"][0],
                       "main_arrows": []} for phase in phases]
    required = fields["secondary"]["allowed"][0]
    return {"live": live, "fields": fields, "joint_states": _unique(phases),
            "secondary_policy": {"required": required,
                "previously_seen": [_card(row) for row in historical
                                    if _identity(row) not in {_identity(row) for row in rows}],
                "retirement_unknown": live and configuration.get("alertPersistenceSeconds") != 0},
            "wire": {"rows": deepcopy(rows), "display": deepcopy(display)}}


def _equivalent_pending_repeat(timeline, capture_ns):
    """Keep a qualified accepted state while only identical input is in flight.

    This does not qualify the pending send or change the counter instrument's
    contract. Complete packet equality preserves the table, display planes and
    other presentation inputs. Mute confirmation is stateful, so its first
    repeated display remains unresolved until the second display is accepted.
    """
    requested = [state for state in timeline["states"]
                 if state["stimulus_requested_ns"] <= capture_ns]
    previous = next((state for state in reversed(requested)
                     if state["all_accepted_ns"] <= capture_ns), None)
    if previous is None:
        return None
    # Earlier partial tables can leave assembly-cache rows behind even after
    # later complete packet sets were sent. This exception supports only a
    # serialized, scoped history; it does not infer recovery of that cache.
    if any(event["packet_id"] in (0x31, 0x43) and event["stimulus_sequence"] is None
           and event["display_requested_ns"] <= capture_ns for event in timeline["accepted"]):
        return None
    history = [state for state in requested
               if state["stimulus_sequence"] <= previous["stimulus_sequence"]]
    if any(left["all_accepted_ns"] > right["stimulus_requested_ns"]
           for left, right in zip(history, history[1:])):
        return None
    repeats = [state for state in requested
               if state["stimulus_sequence"] > previous["stimulus_sequence"]]
    if not repeats or any(state["packet_signature"] != previous["packet_signature"] for state in repeats):
        return None
    members = {previous["stimulus_sequence"], *(state["stimulus_sequence"] for state in repeats)}
    # Qualify the earlier table's assembly as well as the later resend. A
    # foreign row accepted before that table's display could replace its rows;
    # completion of the display alone does not prove the authored table won.
    overlapping = [event for event in timeline["accepted"]
                   if event["display_requested_ns"] <= capture_ns
                   and event["display_accepted_ns"] >= previous["stimulus_requested_ns"]]
    if any(event["stimulus_sequence"] not in members for event in overlapping):
        return None
    prior = counter_expectation_at(timeline, previous["all_accepted_ns"], stealth_enabled=False)
    if (any("unresolved" in field for field in prior["fields"].values())
            or prior["input"].get("display_stimulus_sequence") != previous["stimulus_sequence"]):
        return None
    display = next(event for event in reversed(timeline["accepted"])
                   if event["packet_id"] == 0x31 and event["display_accepted_ns"] <= capture_ns)
    if display["decoded_display"]["mute_bit"] and display["consecutive_muted_displays"] < 2:
        return None
    prior = deepcopy(prior)
    prior["input"]["equivalent_pending_repeat"] = {
        "basis": "Previously qualified complete input remains applicable; every intervening requested "
                 "packet set is byte-identical and cannot change the mute confirmation state.",
        "requested_stimulus_sequences": [state["stimulus_sequence"] for state in repeats],
        "accepted_stimulus_sequence": previous["stimulus_sequence"],
        "accepted_state_ns": previous["all_accepted_ns"],
        "pending_global_tx_sequences": [event["global_tx_sequence"] for event in overlapping
                                        if event["display_accepted_ns"] > capture_ns],
        "pending_delivery_qualified": False,
    }
    return prior


def encounter_expectation_at(timeline, capture_ns, configuration=None):
    """Expected visible radar context at a source-frame timestamp, without pixels.

    A caller-supplied configuration is an assertion that the caller has verified
    its boot/window binding. No configuration is inferred from defaults or pixels.
    """
    _require(type(capture_ns) is int and capture_ns >= 0, "invalid capture timestamp")
    configuration = _configuration(configuration)
    # Supplying false here tests input readiness only. Actual idle field policy
    # below still requires an independently bound stealth=false setting.
    counter = counter_expectation_at(timeline, capture_ns, stealth_enabled=False)
    reason = next((field["unresolved"] for field in counter["fields"].values()
                   if "unresolved" in field), None)
    if reason in ("display transmission is pending at capture time",
                  "alert-table transmission is pending at capture time",
                  "current stimulus notifications were not all accepted by capture time"):
        equivalent = _equivalent_pending_repeat(timeline, capture_ns)
        if equivalent is not None:
            equivalent["input"]["equivalent_pending_repeat"]["strict_counter_unresolved_reason"] = reason
            counter, reason = equivalent, None
    input_info = {**counter["input"], "ready": reason is None,
                  "unresolved": reason, "capture_ns": capture_ns}
    result = {"input": input_info, "live": None, "fields": {
        name: {"unresolved": reason or "no complete input"} for name in FIELDS},
        "joint_states": [], "previous_input": None,
        "basis": "Sampled agreement with host-accepted input, not DUT receipt or a response deadline."}
    if reason:
        return result
    states = timeline["states"]
    current = next((state for state in states if state["stimulus_sequence"] ==
                    input_info.get("stimulus_sequence")), None)
    _require(current is not None, "validated encounter input lacks a corresponding state")
    completed = [state for state in states if state["all_accepted_ns"] <= capture_ns]
    events = [event for event in timeline["accepted"]
              if event["packet_id"] == 0x31 and event["display_accepted_ns"] <= capture_ns]
    latest = events[-1]
    rows = [] if input_info.get("effective_alert_count") == 0 else current["rows"]
    historical = {}
    for state in completed:
        for row in state["rows"]:
            historical[_identity(row)] = row
    result.update(_presentation(rows, latest["decoded_display"],
                                latest["consecutive_muted_displays"] >= 2,
                                configuration, list(historical.values())))
    input_info["age_since_all_accepted_ms"] = (capture_ns - current["all_accepted_ns"]) / 1e6
    prior = next((state for state in reversed(completed)
                  if state["stimulus_sequence"] < current["stimulus_sequence"]
                  and state["packet_signature"] != current["packet_signature"]), None)
    if prior:
        prior_event = next(event for event in reversed(events)
                           if event["stimulus_sequence"] == prior["stimulus_sequence"])
        prior_history = {}
        for state in completed:
            if state["stimulus_sequence"] > prior["stimulus_sequence"]:
                break
            for row in state["rows"]:
                prior_history[_identity(row)] = row
        result["previous_input"] = {"stimulus_sequence": prior["stimulus_sequence"],
            "all_accepted_ns": prior["all_accepted_ns"], **_presentation(
                prior["rows"], prior["display"], prior_event["consecutive_muted_displays"] >= 2,
                configuration, list(prior_history.values()))}
    return result


def _normalize(name, value, partial=False):
    if value is None and partial:
        return None
    if name == "counter_glyph":
        _require(value is None or isinstance(value, str) and value in (*"0123456789", "A", "L", "l"),
                 "invalid literal counter glyph")
    elif name in ("primary_frequency", "frequency"):
        _require(value is None or isinstance(value, str) and
                 re.fullmatch(r"(?:[0-9]{1,2}\.[0-9]{3}|--\.---)", value) is not None,
                 "invalid literal frequency")
    elif name in ("active_bands", "main_arrows"):
        _require(isinstance(value, list), f"invalid literal {name}")
        child = "band" if name == "active_bands" else "direction"
        value = [_normalize(child, item) for item in value]
        _require(len(set(value)) == len(value), f"duplicate literal {name}")
        value = sorted(value)
    elif name in ("main_bars", "bars"):
        _require(type(value) is int and 0 <= value <= 6, "invalid literal bar count")
    elif name == "muted_badge":
        _require(type(value) is bool, "invalid literal muted badge")
    elif name == "band":
        _require(isinstance(value, str) and value.lower() in ("x", "k", "ka"), "invalid literal radar band")
        value = {"x": "X", "k": "K", "ka": "Ka"}[value.lower()]
    elif name == "direction":
        _require(isinstance(value, str) and value.lower() in ("front", "side", "rear"),
                 "invalid literal direction")
        value = value.lower()
    elif name == "secondary":
        _require(isinstance(value, list) and len(value) <= 2, "invalid literal secondary card list")
        cards = []
        for card in value:
            _require(isinstance(card, dict) and set(card) >= {"band", "frequency", "direction", "bars"},
                     "secondary card lacks associated fields")
            cards.append({key: _normalize(key, card[key], partial=True)
                          for key in ("band", "frequency", "direction", "bars")})
        value = cards
    return value


def _observed(name, observed):
    _require(isinstance(observed, dict), "missing literal observation")
    state = observed.get("state")
    _require(state in ("readable", "absent", "unreadable", "ambiguous"), "invalid observation state")
    if state in ("unreadable", "ambiguous"):
        raise EncounterEvidenceError(observed.get("reason") or f"literal {state}")
    if state == "absent":
        _require(observed.get("value") is None, "absent observation also supplies a value")
        return {"active_bands": [], "main_arrows": [], "main_bars": 0,
                "secondary": [], "muted_badge": False}.get(name)
    _require("value" in observed, "readable observation lacks value")
    _require(observed["value"] is not None, "readable observation has no literal value")
    return _normalize(name, observed["value"])


def _secondary_check(policy, cards):
    required = policy["required"]
    compatible = lambda expected, actual: all(value is None or expected[key] == value
                                               for key, value in actual.items())
    # Two visible slots: find the largest association-preserving matching.
    def match(index, used):
        if index == len(required):
            return []
        best = match(index + 1, used)
        for slot, card in enumerate(cards):
            if slot not in used and compatible(required[index], card):
                candidate = [(index, slot)] + match(index + 1, used | {slot})
                score = lambda pairs: (len(pairs), sum(all(value is not None for value in cards[slot].values())
                                                       for _, slot in pairs))
                if score(candidate) > score(best):
                    best = candidate
        return best
    matched = match(0, set())
    if len(matched) < len(required):
        return {"status": "DIFFERENCE", "reason": "required live card missing or wrong associated fields"}
    used = {slot for _, slot in matched}
    unresolved = any(any(value is None for value in card.values()) for card in cards)
    extras = [card for slot, card in enumerate(cards) if slot not in used]
    for card in extras:
        if any(value is None for value in card.values()):
            continue
        if not policy["retirement_unknown"] or card not in policy["previously_seen"]:
            return {"status": "DIFFERENCE", "reason": "unexpected card or wrong historical association"}
    if unresolved:
        return {"status": "UNRESOLVED", "reason": "partial literal card leaves associated content unreadable"}
    if extras:
        return {"status": "CONDITIONAL", "reason": "retired card visible; DUT-side persistence expiry unestablished"}
    return {"status": "MATCH"}


def _compare_fields(expected, observed):
    checks = {}
    normalized = {}
    for name in FIELDS:
        spec = expected["fields"][name]
        if "unresolved" in spec:
            checks[name] = {"status": "UNRESOLVED", "reason": spec["unresolved"]}
            continue
        try:
            actual = _observed(name, observed.get(name))
            normalized[name] = actual
            if name == "secondary":
                check = _secondary_check(expected["secondary_policy"], actual)
            else:
                allowed = [_normalize(name, value) for value in spec["allowed"]]
                check = {"status": "MATCH" if actual in allowed else "DIFFERENCE"}
                if check["status"] == "DIFFERENCE":
                    check["reason"] = "literal content differs from complete accepted input"
            checks[name] = {**check, "observed": actual, "expected": spec}
        except EncounterEvidenceError as error:
            checks[name] = {"status": "UNRESOLVED", "reason": str(error)}
            # An identified card contradicts a no-card target even if its
            # direction or strength cannot be read. Preserve those unknowns;
            # this establishes presence, never a complete card or a match.
            reading = observed.get(name)
            if (name == "secondary" and spec.get("allowed") == [[]]
                    and expected["secondary_policy"].get("retirement_unknown") is False
                    and isinstance(reading, dict)
                    and reading.get("state") in ("unreadable", "ambiguous")):
                try:
                    partial = _normalize("secondary", reading.get("partial_cards"))
                except EncounterEvidenceError:
                    continue
                if any(card["band"] is not None and card["frequency"] is not None
                       for card in partial):
                    checks[name] = {
                        "status": "DIFFERENCE",
                        "reason": "identified secondary card visible when accepted input permits none; "
                                  "remaining card details are unresolved",
                        "observed": partial, "expected": spec,
                    }
    joint = {"status": "UNRESOLVED", "reason": "joint blink fields are not all readable/resolved"}
    phases = expected.get("joint_states", [])
    keys = set(phases[0]) if phases else set()
    if keys and keys <= normalized.keys():
        matched = next((index for index, phase in enumerate(phases)
                        if all(normalized[key] == _normalize(key, phase[key]) for key in keys)), None)
        joint = {"status": "MATCH" if matched is not None else "DIFFERENCE", "fields": sorted(keys)}
        if matched is not None:
            joint["state_id"] = f"phase-{matched + 1}"
        else:
            joint["reason"] = "counter/band/arrow combination is not one permitted shared blink phase"
    elif not keys:
        joint = {"status": "NOT_EVALUATED", "reason": "no resolved joint blink scope"}
    return checks, joint


def compare_sample(expected, observed, role="held"):
    """Compare literal independent observations without hiding supported differences.

    The seven-field denominator is fixed. Joint blink consistency is a separate
    relationship check; consumers must respect overall status, not only counts.
    """
    _require(role in ("held", "transition"), "invalid encounter sample role")
    _require(isinstance(observed, dict), "invalid observation record")
    observed = observed.get("fields", observed)
    _require(isinstance(observed, dict), "invalid observation fields")
    checks, joint = _compare_fields(expected, observed)
    if role == "transition":
        prior = expected.get("previous_input")
        prior_checks, prior_joint = _compare_fields(prior, observed) if prior else ({}, {})
        for name, check in checks.items():
            if check["status"] == "DIFFERENCE":
                check["status"] = ("PREVIOUS_INPUT_STATE" if prior_checks.get(name, {}).get("status") == "MATCH"
                                   else "TRANSITION_DIFFERENCE")
        if joint["status"] == "DIFFERENCE":
            joint["status"] = ("PREVIOUS_INPUT_STATE" if prior_joint.get("status") == "MATCH"
                               else "TRANSITION_DIFFERENCE")
    counts = dict(Counter(check["status"] for check in checks.values()))
    statuses = {check["status"] for check in checks.values()} | {joint["status"]}
    status = ("DIFFERENCE" if "DIFFERENCE" in statuses else
              "TRANSITION_OBSERVATION" if statuses & {"PREVIOUS_INPUT_STATE", "TRANSITION_DIFFERENCE"} else
              "INCONCLUSIVE" if statuses & {"UNRESOLVED", "CONDITIONAL"} else "MATCH")
    return {"status": status, "role": role, "checks": checks, "counts": counts,
            "joint_state": joint, "field_count": len(FIELDS),
            "basis": "Sampled host-input agreement; transition differences establish no response deadline."}
