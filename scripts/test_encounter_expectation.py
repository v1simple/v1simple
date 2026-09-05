#!/usr/bin/env python3
"""Independent wire/literal fixtures for the sampled encounter instrument."""
import copy
import hashlib
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from bench.encounter_expectation import (FIELDS, EncounterEvidenceError,
    build_encounter_timeline, compare_sample, encounter_expectation_at)


def packet(packet_id, payload):
    raw = bytes([0xAA, 0xD8, 0xEA, packet_id, len(payload) + 1, *payload])
    return (raw + bytes([sum(raw) & 255, 0xAB])).hex()


def alert(band="k", frequency=24150, direction="FRONT", rssi=1, priority=True):
    return {"band": band, "frequencyMHz": frequency, "direction": direction,
            "rssi": rssi, "priority": priority}


def recording(states):
    """states contain rows and independently specified display payload bytes."""
    scenario = {"schemaVersion": 1, "samples": []}
    stimuli, delivery = [], []
    tx = 0
    for index, (rows, display) in enumerate(states):
        sequence, start = index + 1, (index + 1) * 1_000_000_000
        scenario["samples"].append({"sourceIndex": index, "offsetSeconds": index,
                                    "alerts": [{k: v for k, v in row.items() if k != "rssi"} for row in rows]})
        payloads = []
        for number, row in enumerate(rows, 1):
            frequency, raw = row["frequencyMHz"], row["rssi"]
            mask = {"x": 8, "k": 4, "ka": 2}[row["band"]]
            mask |= {"FRONT": 32, "SIDE": 64, "REAR": 128}[row["direction"]]
            payloads.append(packet(0x43, [number * 16 + len(rows), frequency >> 8, frequency & 255,
                raw, raw, mask, 128 if row["priority"] else 0]))
        if not rows:
            payloads.append(packet(0x43, [0] * 7))
        payloads.append(packet(0x31, display))
        notifications = []
        for ordinal, payload in enumerate(payloads):
            tx += 1
            kind = "display_frame" if ordinal == len(payloads) - 1 else "alert_row"
            notifications.append(dict(ordinal=ordinal, channel="display_short", kind=kind, bytesHex=payload))
            base = dict(schemaVersion=3, globalTxSequence=tx, payloadHex=payload,
                        payloadSha256=hashlib.sha256(bytes.fromhex(payload)).hexdigest(),
                        characteristic="B2CE", stimulusSequence=sequence, emissionOrdinal=ordinal)
            request = start + (ordinal * 10 + 1) * 1_000_000
            accepted = start + (ordinal * 10 + 5) * 1_000_000
            delivery.extend([dict(base, state="notification_requested", hostMonotonicNs=request),
                             dict(base, state="notification_accepted", hostMonotonicNs=accepted,
                                  attemptedHostMonotonicNs=accepted - 1)])
        stimuli.append(dict(schemaVersion=2, state="stimulus_requested", stimulusSequence=sequence,
                            sourceIndex=index, replayOffsetSeconds=index, requestedHostMonotonicNs=start,
                            notifications=notifications, expected={"not": "an oracle"}))
    return scenario, stimuli, delivery


def single():
    return recording([([alert()], [6, 6, 1, 0x24, 0x24, 12, 12, 0x40])])


def literals(**changes):
    # Independent known screen context, never copied from expected output.
    values = dict(counter_glyph="1", primary_frequency="24.150", active_bands=["K"],
                  main_arrows=["front"], main_bars=1, secondary=[], muted_badge=False)
    values.update(changes)
    return {name: {"state": "readable", "value": value} for name, value in values.items()}


class EncounterExpectationTests(unittest.TestCase):
    def expected(self, data=None, capture=1_500_000_000, config=None):
        return encounter_expectation_at(build_encounter_timeline(*(data or single())), capture, config)

    def test_live_all_fields_match_and_source_inputs_unchanged(self):
        data = single()
        original = copy.deepcopy(data)
        expected = self.expected(data)
        result = compare_sample(expected, literals())
        self.assertEqual(result["status"], "MATCH")
        self.assertEqual(result["counts"], {"MATCH": 7})
        self.assertEqual(result["joint_state"]["status"], "MATCH")
        self.assertEqual(result["joint_state"]["state_id"], "phase-1")
        self.assertEqual(data, original)

    def test_wrong_live_field_survives_other_unreadable_fields(self):
        observed = literals(primary_frequency="35.500")
        observed["main_arrows"] = {"state": "unreadable", "reason": "glare"}
        result = compare_sample(self.expected(), observed)
        self.assertEqual(result["status"], "DIFFERENCE")
        self.assertEqual(result["checks"]["primary_frequency"]["status"], "DIFFERENCE")
        self.assertEqual(result["checks"]["main_arrows"]["status"], "UNRESOLVED")
        self.assertEqual(sum(result["counts"].values()), 7)

    def test_absent_required_information_is_not_reader_abstention(self):
        for field in ("counter_glyph", "primary_frequency", "active_bands", "main_arrows", "main_bars"):
            observed = literals()
            observed[field] = {"state": "absent"}
            with self.subTest(field=field):
                self.assertEqual(compare_sample(self.expected(), observed)["checks"][field]["status"], "DIFFERENCE")
        observed = literals()
        observed["secondary"] = {"state": "absent"}
        observed["muted_badge"] = {"state": "absent"}
        self.assertEqual(compare_sample(self.expected(), observed)["status"], "MATCH")

    def test_missing_ambiguous_malformed_literals_remain_unresolved(self):
        for bad in (None, {"state": "ambiguous"}, {"state": "readable", "value": True},
                    {"state": "absent", "value": "1"}, {"state": "invented", "value": "1"}):
            observed = literals()
            observed["counter_glyph"] = bad
            self.assertEqual(compare_sample(self.expected(), observed)["checks"]["counter_glyph"]["status"], "UNRESOLVED")

    def test_main_strength_and_secondary_strength_use_distinct_maps(self):
        data = recording([([alert("ka", 34700, "FRONT", 186),
                            alert("k", 24150, "SIDE", 194, False)],
                           [91, 91, 127, 0x22, 0x22, 12, 12, 0x40])])
        result = compare_sample(self.expected(data), literals(counter_glyph="2", primary_frequency="34.700",
            active_bands=["Ka"], main_bars=6,
            secondary=[dict(band="K", frequency="24.150", direction="SIDE", bars=6)]))
        self.assertEqual(result["status"], "MATCH")
        # Five V1 RSSI bars project to FOUR secondary cells; bitmap31 clamps to FIVE main cells.
        data = recording([([alert("ka", 34700), alert("k", 24150, "SIDE", 164, False)],
                           [91, 91, 31, 0x22, 0x22, 12, 12, 0x40])])
        expected = self.expected(data)
        self.assertEqual(expected["fields"]["main_bars"]["allowed"], [5])
        self.assertEqual(expected["secondary_policy"]["required"][0]["bars"], 4)

    def two_cards(self):
        data = recording([([alert("ka", 34700), alert("k", 24150, "SIDE", 164, False),
                            alert("ka", 35500, "REAR", 144, False)],
                           [79, 79, 7, 0x22, 0x22, 12, 12, 0x40])])
        expected = self.expected(data)
        observed = literals(counter_glyph="3", primary_frequency="34.700", active_bands=["Ka"], main_bars=3,
                            secondary=[dict(band="K", frequency="24.150", direction="side", bars=4),
                                       dict(band="Ka", frequency="35.500", direction="rear", bars=2)])
        return expected, observed

    def test_cards_compare_complete_associations_and_ignore_slot_order(self):
        expected, observed = self.two_cards()
        observed["secondary"]["value"].reverse()
        self.assertEqual(compare_sample(expected, observed)["status"], "MATCH")
        cards = observed["secondary"]["value"]
        cards[0]["direction"], cards[1]["direction"] = cards[1]["direction"], cards[0]["direction"]
        self.assertEqual(compare_sample(expected, observed)["checks"]["secondary"]["status"], "DIFFERENCE")

    def test_missing_card_not_hidden_by_partial_other_card(self):
        expected, observed = self.two_cards()
        observed["secondary"]["value"].pop()
        observed["secondary"]["value"][0]["bars"] = None
        self.assertEqual(compare_sample(expected, observed)["checks"]["secondary"]["status"], "DIFFERENCE")
        observed["secondary"] = {"state": "absent"}
        self.assertEqual(compare_sample(expected, observed)["checks"]["secondary"]["status"], "DIFFERENCE")

    def test_partial_associated_card_abstains_without_inventing_values(self):
        expected, observed = self.two_cards()
        observed["secondary"]["value"][0]["bars"] = None
        self.assertEqual(compare_sample(expected, observed)["checks"]["secondary"]["status"], "UNRESOLVED")
        observed["secondary"]["value"][0]["frequency"] = "24.125"
        self.assertEqual(compare_sample(expected, observed)["checks"]["secondary"]["status"], "DIFFERENCE")

    def test_identified_unreadable_card_contradicts_no_card_target(self):
        expected = self.expected(config={"alertPersistenceSeconds": 0})
        observed = literals()
        observed["secondary"] = {
            "state": "unreadable", "value": None,
            "reason": "card direction and bars are not fully readable",
            "partial_cards": [dict(band="Ka", frequency="34.700", direction=None, bars=None)],
        }
        original = copy.deepcopy(observed)
        result = compare_sample(expected, observed)
        self.assertEqual(result["status"], "DIFFERENCE")
        check = result["checks"]["secondary"]
        self.assertEqual(check["status"], "DIFFERENCE")
        self.assertEqual(check["observed"][0]["direction"], None)
        self.assertEqual(check["observed"][0]["bars"], None)
        self.assertEqual(observed, original)

        # A permitted card with unreadable details cannot become a match, and
        # unestablished retirement cannot be treated as expired by host time.
        required, _ = self.two_cards()
        for allowed_target in (required, self.expected()):
            self.assertEqual(compare_sample(allowed_target, observed)["checks"]["secondary"]["status"],
                             "UNRESOLVED")

    def test_unidentified_or_malformed_card_does_not_prove_presence(self):
        expected = self.expected(config={"alertPersistenceSeconds": 0})
        card = dict(band="Ka", frequency="34.700", direction=None, bars=None)
        for partial in (None, [], [{}], [dict(card, band=None)], [dict(card, frequency=None)],
                        [dict(card, frequency="unknown")], [dict(card, bars=8)]):
            with self.subTest(partial=partial):
                observed = literals()
                observed["secondary"] = {"state": "unreadable", "partial_cards": partial}
                self.assertEqual(compare_sample(expected, observed)["checks"]["secondary"]["status"],
                                 "UNRESOLVED")
        observed["secondary"] = {"state": "invalid", "partial_cards": [card]}
        self.assertEqual(compare_sample(expected, observed)["checks"]["secondary"]["status"],
                         "UNRESOLVED")

    def test_partial_extra_does_not_displace_a_complete_required_card(self):
        data = recording([([alert("ka", 34700), alert("k", 24150, "SIDE", 164, False)],
                           [91, 91, 31, 0x22, 0x22, 12, 12, 0x40])])
        complete = dict(band="K", frequency="24.150", direction="side", bars=4)
        partial = dict(complete, bars=None)
        observed = literals(counter_glyph="2", primary_frequency="34.700", active_bands=["Ka"],
                            main_bars=5, secondary=[partial, complete])
        result = compare_sample(self.expected(data), observed)
        self.assertEqual(result["checks"]["secondary"]["status"], "UNRESOLVED")

    def test_blink_joint_phase_rejects_impossible_independent_matches(self):
        # Shared on phase is counter1/K/front; shared off is blank/no-band/no-arrow.
        data = recording([([alert()], [6, 0, 1, 0x24, 0, 12, 12, 0x40])])
        expected = self.expected(data)
        impossible = literals(active_bands=[], main_arrows=[])
        result = compare_sample(expected, impossible)
        self.assertEqual(result["counts"], {"MATCH": 7})
        self.assertEqual(result["joint_state"]["status"], "DIFFERENCE")
        self.assertEqual(result["status"], "DIFFERENCE")
        impossible["counter_glyph"] = {"state": "absent"}
        off = compare_sample(expected, impossible)
        self.assertEqual(off["status"], "MATCH")
        self.assertEqual(off["joint_state"]["state_id"], "phase-2")

    def test_priority_arrow_setting_unknown_does_not_invent_projection(self):
        data = recording([([alert()], [6, 6, 1, 0x64, 0x64, 12, 12, 0x40])])
        self.assertIn("unresolved", self.expected(data)["fields"]["main_arrows"])
        self.assertEqual(self.expected(data, config={"priorityArrowOnly": True})["fields"]["main_arrows"],
                         {"allowed": [["front"]]})
        self.assertEqual(self.expected(data, config={"priorityArrowOnly": False})["fields"]["main_arrows"],
                         {"allowed": [["front", "side"]]})

    def test_input_pending_keeps_entire_denominator_unresolved(self):
        for timestamp in (0, 1_000_000_000, 1_014_000_000):
            result = compare_sample(self.expected(capture=timestamp), literals())
            self.assertEqual(result["counts"], {"UNRESOLVED": 7})
            self.assertEqual(result["status"], "INCONCLUSIVE")
        self.assertTrue(self.expected(capture=1_015_000_000)["input"]["ready"])

    def test_identical_pending_repeat_uses_prior_accepted_state_and_retains_send_provenance(self):
        from bench.counter_expectation import counter_expectation_at
        data = recording([([alert()], [6, 6, 1, 0x24, 0x24, 12, 12, 0x40])] * 2)
        timeline = build_encounter_timeline(*data)
        original = copy.deepcopy(timeline)
        for capture in (2_000_000_000, 2_003_000_000, 2_014_000_000):
            expected = encounter_expectation_at(timeline, capture)
            self.assertTrue(expected["input"]["ready"], expected)
            self.assertEqual(compare_sample(expected, literals())["status"], "MATCH")
            self.assertEqual(expected["input"]["stimulus_sequence"], 1)
            self.assertEqual(expected["input"]["capture_ns"], capture)
            self.assertEqual(expected["input"]["all_accepted_ns"], 1_015_000_000)
            proof = expected["input"]["equivalent_pending_repeat"]
            self.assertEqual(proof["requested_stimulus_sequences"], [2])
            self.assertFalse(proof["pending_delivery_qualified"])
            strict = counter_expectation_at(timeline, capture, stealth_enabled=False)
            self.assertTrue(all("unresolved" in field for field in strict["fields"].values()))
        self.assertEqual(timeline, original)

    def test_partial_identical_table_does_not_invent_a_different_presentation(self):
        rows = [alert("ka", 34700), alert("k", 24150, "SIDE", 144, False)]
        data = recording([(rows, [91, 91, 1, 0x22, 0x22, 12, 12, 0x40])] * 2)
        timeline = build_encounter_timeline(*data)
        prior = encounter_expectation_at(timeline, 1_500_000_000)
        # One row is accepted; the second row and display are not yet sent.
        pending = encounter_expectation_at(timeline, 2_006_000_000)
        self.assertTrue(pending["input"]["ready"], pending)
        for key in ("fields", "joint_states", "secondary_policy"):
            self.assertEqual(pending[key], prior[key])

    def test_changed_row_or_display_never_borrows_prior_accepted_state(self):
        first = ([alert()], [6, 6, 1, 0x24, 0x24, 12, 12, 0x40])
        for second in (([alert(frequency=24200)], first[1]),
                       ([alert(rssi=194)], first[1]),
                       (first[0], [6, 6, 3, 0x24, 0x24, 12, 12, 0x40])):
            expected = self.expected(recording([first, second]), capture=2_014_000_000)
            self.assertFalse(expected["input"]["ready"], expected)
            self.assertEqual(compare_sample(expected, literals())["counts"], {"UNRESOLVED": 7})

    def test_identical_pending_mute_repeat_cannot_supply_second_confirmation(self):
        data = recording([([alert()], [6, 6, 1, 0x34, 0x34, 13, 12, 0x40])] * 3)
        timeline = build_encounter_timeline(*data)
        second = encounter_expectation_at(timeline, 2_014_000_000)
        self.assertFalse(second["input"]["ready"], second)
        third = encounter_expectation_at(timeline, 3_014_000_000)
        self.assertTrue(third["input"]["ready"], third)
        self.assertEqual(third["input"]["stimulus_sequence"], 2)
        self.assertEqual(third["fields"]["muted_badge"], {"allowed": [True]})

    def test_changed_intervening_request_cannot_be_hidden_by_later_identical_repeat(self):
        first = ([alert()], [6, 6, 1, 0x24, 0x24, 12, 12, 0x40])
        data = recording([first, ([alert(frequency=24200)], first[1]), first])
        # The changed middle state is still queued while an identical copy of
        # the original state is requested. Equality of only the endpoints is
        # insufficient: either in-flight packet set could reach the display.
        for event in data[2]:
            if event["stimulusSequence"] == 2 and event["state"] == "notification_accepted":
                event["hostMonotonicNs"] += 1_200_000_000
                event["attemptedHostMonotonicNs"] += 1_200_000_000
        expected = self.expected(data, capture=3_014_000_000)
        self.assertFalse(expected["input"]["ready"], expected)

    def test_intervening_unscoped_packet_blocks_identical_repeat_equivalence(self):
        for request, accepted in ((1_500_000_000, 1_600_000_000),
                                  (2_010_000_000, 2_100_000_000)):
            data = recording([([alert()], [6, 6, 1, 0x24, 0x24, 12, 12, 0x40])] * 2)
            extra = {key: value for key, value in data[2][-1].items()
                     if key not in ("state", "hostMonotonicNs", "attemptedHostMonotonicNs")}
            extra.update(globalTxSequence=5, stimulusSequence=None, emissionOrdinal=None)
            data[2].extend([dict(extra, state="notification_requested", hostMonotonicNs=request),
                            dict(extra, state="notification_accepted", hostMonotonicNs=accepted,
                                 attemptedHostMonotonicNs=accepted - 1)])
            expected = self.expected(data, capture=2_014_000_000)
            self.assertFalse(expected["input"]["ready"], expected)

    def test_unscoped_table_inside_prior_assembly_blocks_identical_repeat_equivalence(self):
        first = ([alert()], [6, 6, 1, 0x24, 0x24, 12, 12, 0x40])
        data = recording([first, first])
        # The scoped row publishes 24.150 at 1.005 s, but a complete unscoped
        # one-row table publishes 24.200 before that state's display at 1.015 s.
        # Acceptance of the final display cannot certify the earlier table.
        for event in data[2]:
            if event["globalTxSequence"] > 1:
                event["globalTxSequence"] += 1
        payload = packet(0x43, [0x11, 24200 >> 8, 24200 & 255, 1, 1, 0x24, 0x80])
        extra = dict(schemaVersion=3, globalTxSequence=2, payloadHex=payload,
                     payloadSha256=hashlib.sha256(bytes.fromhex(payload)).hexdigest(),
                     characteristic="B2CE", stimulusSequence=None, emissionOrdinal=None)
        data[2].extend([
            dict(extra, state="notification_requested", hostMonotonicNs=1_007_000_000),
            dict(extra, state="notification_accepted", hostMonotonicNs=1_010_000_000,
                 attemptedHostMonotonicNs=1_009_999_999),
        ])
        data[2].sort(key=lambda event: event["hostMonotonicNs"])
        expected = self.expected(data, capture=2_003_000_000)
        self.assertFalse(expected["input"]["ready"], expected)

    def test_unscoped_cached_row_before_prior_assembly_blocks_repeat_equivalence(self):
        rows = [alert("ka", 34700), alert("k", 24150, "SIDE", 144, False)]
        data = recording([(rows, [91, 91, 1, 0x22, 0x22, 12, 12, 0x40])] * 2)
        for event in data[2]:
            event["globalTxSequence"] += 1
        # A fresh row2/count2 exists before the scoped request. Its row1 can
        # publish with that cached row, clear the cache, and leave the scoped
        # row2 partial. The later display does not establish table provenance.
        payload = packet(0x43, [0x22, 24200 >> 8, 24200 & 255, 144, 144, 0x44, 0])
        extra = dict(schemaVersion=3, globalTxSequence=1, payloadHex=payload,
                     payloadSha256=hashlib.sha256(bytes.fromhex(payload)).hexdigest(),
                     characteristic="B2CE", stimulusSequence=None, emissionOrdinal=None)
        data[2].extend([
            dict(extra, state="notification_requested", hostMonotonicNs=990_000_000),
            dict(extra, state="notification_accepted", hostMonotonicNs=995_000_000,
                 attemptedHostMonotonicNs=994_999_999),
        ])
        data[2].sort(key=lambda event: event["hostMonotonicNs"])
        expected = self.expected(data, capture=2_003_000_000)
        self.assertFalse(expected["input"]["ready"], expected)

    def test_completed_historical_interleaving_blocks_repeat_equivalence(self):
        display = [91, 91, 1, 0x22, 0x22, 12, 12, 0x40]
        def rows(frequency):
            return [alert("ka", 34700), alert("k", frequency, "SIDE", 144, False)]
        data = recording([(rows(24150), display), (rows(24200), display),
                          (rows(24300), display), (rows(24300), display)])
        data[1][0]["requestedHostMonotonicNs"] = 1_990_000_000
        # Assemblies 1 and 2 overlap but both finish before assembly 3 starts.
        # Row1 of 2 plus row2 of 1 can publish and clear the cache; row2 of 2
        # then survives to combine with row1 of 3. Completion timestamps alone
        # cannot establish a clean baseline for the pending fourth repeat.
        times = ((1_991_000_000, 1_995_000_000),
                 (2_006_000_000, 2_010_000_000),
                 (2_016_000_000, 2_020_000_000))
        for event in data[2]:
            if event["stimulusSequence"] == 1:
                requested, accepted = times[event["emissionOrdinal"]]
                event["hostMonotonicNs"] = requested if event["state"] == "notification_requested" else accepted
                if event["state"] == "notification_accepted":
                    event["attemptedHostMonotonicNs"] = accepted - 1
        ordered = sorted((event for event in data[2] if event["state"] == "notification_requested"),
                         key=lambda event: event["hostMonotonicNs"])
        transmissions = {(event["stimulusSequence"], event["emissionOrdinal"]): index
                         for index, event in enumerate(ordered, 1)}
        for event in data[2]:
            event["globalTxSequence"] = transmissions[event["stimulusSequence"], event["emissionOrdinal"]]
        data[2].sort(key=lambda event: event["hostMonotonicNs"])
        expected = self.expected(data, capture=4_003_000_000)
        self.assertFalse(expected["input"]["ready"], expected)

    def test_idle_policy_unknown_and_bound_zero_persistence(self):
        data = recording([([], [56, 56, 0, 0, 0, 12, 12, 0x40])])
        observed = literals(counter_glyph="L", primary_frequency="--.---", active_bands=[], main_arrows=[], main_bars=0)
        result = compare_sample(self.expected(data), observed)
        self.assertEqual(result["counts"], {"UNRESOLVED": 4, "MATCH": 3})
        result = compare_sample(self.expected(data, config={"stealthEnabled": False,
                                                          "alertPersistenceSeconds": 0}), observed)
        self.assertEqual(result["status"], "MATCH")
        expected = self.expected(data, config={"stealthEnabled": False, "alertPersistenceSeconds": 1})
        self.assertIn("unresolved", expected["fields"]["primary_frequency"])

    def test_idle_nonzero_leds_follow_resting_renderer_only_when_bound(self):
        # Resting uses the current bitmap and steady image1 bands, even when
        # the table is empty. Persisted mode instead clears the strength.
        data = recording([([], [56, 56, 1, 0x24, 0, 12, 12, 0x40])])
        expected = self.expected(data)
        self.assertIn("unresolved", expected["fields"]["main_bars"])
        observed = literals(counter_glyph="L", primary_frequency="--.---",
                            active_bands=["K"], main_arrows=[], main_bars=1)
        result = compare_sample(self.expected(data, config={"stealthEnabled": False,
                                                           "alertPersistenceSeconds": 0}), observed)
        self.assertEqual(result["status"], "MATCH")
        self.assertEqual(result["joint_state"]["status"], "MATCH")
        observed["active_bands"]["value"] = []
        self.assertEqual(compare_sample(self.expected(data, config={"stealthEnabled": False,
                                                                   "alertPersistenceSeconds": 0}), observed)
                         ["checks"]["active_bands"]["status"], "DIFFERENCE")

    def test_near_same_band_rows_refused_in_distinct_card_scope(self):
        # Firmware suppresses identity matches within 2 MHz and maintains card
        # continuity within 5 MHz, so these rows do not imply distinct cards.
        for separation in (1, 2, 5):
            data = recording([([alert(), alert("k", 24150 + separation, "SIDE", 136, False)],
                               [91, 91, 1, 0x24, 0x24, 12, 12, 0x40])])
            with self.subTest(separation=separation), self.assertRaisesRegex(
                    EncounterEvidenceError, "within 5 MHz"):
                build_encounter_timeline(*data)
        data = recording([([alert(), alert("k", 24156, "SIDE", 136, False)],
                           [91, 91, 1, 0x24, 0x24, 12, 12, 0x40])])
        self.assertEqual(len(self.expected(data)["secondary_policy"]["required"]), 1)

    def test_zero_frequency_radar_is_not_a_renderable_ordinary_alert(self):
        data = recording([([alert(frequency=0)], [6, 6, 1, 0x24, 0x24, 12, 12, 0x40])])
        with self.assertRaisesRegex(EncounterEvidenceError, "zero-frequency radar"):
            build_encounter_timeline(*data)

    def test_historical_extra_is_conditional_not_matching_or_expired_by_host_time(self):
        rows = [alert("ka", 34700), alert("k", 24150, "SIDE", 164, False)]
        data = recording([(rows, [91, 91, 7, 0x22, 0x22, 12, 12, 0x40]),
                          ([rows[0]], [6, 6, 7, 0x22, 0x22, 12, 12, 0x40])])
        observed = literals(primary_frequency="34.700", active_bands=["Ka"], main_bars=3,
                            secondary=[dict(band="K", frequency="24.150", direction="side", bars=4)])
        expected = self.expected(data, capture=20_000_000_000)
        self.assertEqual(compare_sample(expected, observed)["checks"]["secondary"]["status"], "CONDITIONAL")
        expected = self.expected(data, capture=20_000_000_000, config={"alertPersistenceSeconds": 0})
        self.assertEqual(compare_sample(expected, observed)["checks"]["secondary"]["status"], "DIFFERENCE")
        observed["secondary"]["value"][0]["direction"] = "rear"
        self.assertEqual(compare_sample(self.expected(data, capture=20_000_000_000), observed)
                         ["checks"]["secondary"]["status"], "DIFFERENCE")

    def test_transition_difference_reports_prior_input_without_a_failure_deadline(self):
        data = recording([([alert()], [6, 6, 1, 0x24, 0x24, 12, 12, 0x40]),
                          ([alert("ka", 35500)], [6, 6, 1, 0x22, 0x22, 12, 12, 0x40])])
        expected = self.expected(data, capture=2_050_000_000)
        result = compare_sample(expected, literals(), role="transition")
        self.assertEqual(result["status"], "TRANSITION_OBSERVATION")
        self.assertEqual(result["checks"]["primary_frequency"]["status"], "PREVIOUS_INPUT_STATE")
        self.assertEqual(compare_sample(expected, literals(), role="held")["status"], "DIFFERENCE")
        observed = literals(primary_frequency="24.125")
        self.assertEqual(compare_sample(expected, observed, role="transition")["checks"]
                         ["primary_frequency"]["status"], "TRANSITION_DIFFERENCE")

    def test_mute_requires_two_accepted_display_packets_but_release_is_immediate(self):
        data = recording([([alert()], [6, 6, 1, 0x34, 0x34, 13, 12, 0x40]),
                          ([alert()], [6, 6, 1, 0x34, 0x34, 13, 12, 0x40]),
                          ([alert()], [6, 6, 1, 0x24, 0x24, 12, 12, 0x40])])
        timeline = build_encounter_timeline(*data)
        for capture, muted in ((1_500_000_000, False), (2_500_000_000, True), (3_500_000_000, False)):
            self.assertEqual(encounter_expectation_at(timeline, capture)["fields"]["muted_badge"],
                             {"allowed": [muted]})

    def test_invalid_source_delivery_and_unsupported_presentation_refused(self):
        data = single()
        data[2][-1]["payloadSha256"] = "0" * 64
        with self.assertRaises(EncounterEvidenceError):
            build_encounter_timeline(*data)
        for display in ([6, 6, 2, 0x24, 0x24, 12, 12, 0x40],
                        [6, 6, 1, 0x24, 0x64, 12, 12, 0x40],
                        [6, 6, 1, 0x25, 0x25, 12, 12, 0x40]):
            with self.assertRaises(EncounterEvidenceError):
                build_encounter_timeline(*recording([([alert()], display)]))
        data = single()
        data[0]["samples"][0]["alerts"][0]["strength"] = 8
        with self.assertRaises(EncounterEvidenceError):
            build_encounter_timeline(*data)

    def test_invalid_config_and_role_refused(self):
        for config in ({"stealthEnabled": 0}, {"priorityArrowOnly": "false"},
                       {"alertPersistenceSeconds": True}, {"alertPersistenceSeconds": 6}):
            with self.assertRaises(EncounterEvidenceError):
                self.expected(config=config)
        with self.assertRaises(EncounterEvidenceError):
            compare_sample(self.expected(), literals(), role="deadline")


if __name__ == "__main__":
    unittest.main()
