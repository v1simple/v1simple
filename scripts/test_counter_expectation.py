#!/usr/bin/env python3
"""Independent packet fixtures for the sampled count/mode input instrument."""

import copy
import hashlib
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent / "bench"))
from counter_expectation import (CounterEvidenceError, build_counter_timeline,
                                 counter_expectation_at, decode_counter_packet)


def packet(packet_id, payload, destination=0xD8, origin=0xEA):
    # ESP framing: length includes checksum; checksum covers preceding bytes.
    data = bytes([0xAA, destination, origin, packet_id, len(payload) + 1, *payload])
    return (data + bytes([sum(data) & 255, 0xAB])).hex()


def display(first, second=None):
    return packet(0x31, [first, first if second is None else second, 1, 0x24, 0x24, 12, 12, 0x40])


def emission(tx, payload, requested, accepted, sequence=None, ordinal=None):
    base = dict(schemaVersion=3, globalTxSequence=tx, payloadHex=payload,
                payloadSha256=hashlib.sha256(bytes.fromhex(payload)).hexdigest(), characteristic="B2CE")
    if sequence is not None:
        base.update(stimulusSequence=sequence, emissionOrdinal=ordinal)
    return [dict(base, state="notification_requested", hostMonotonicNs=requested),
            dict(base, state="notification_accepted", hostMonotonicNs=accepted,
                 attemptedHostMonotonicNs=accepted - 1)]


def fixture():
    alert = dict(band="k", bandMask=4, direction="FRONT", directionMask=0x20,
                 frequencyMHz=24150, priority=True)
    samples = [dict(sourceIndex=0, offsetSeconds=0, alerts=[alert]),
               dict(sourceIndex=1, offsetSeconds=1, alerts=[])]
    scenario = dict(schemaVersion=1, samples=samples)
    stimuli, delivery = [], []
    for index, masks in enumerate(([0x11, 0x5E, 0x56, 1, 0, 0x24, 0x80], [0] * 7)):
        start = (index + 1) * 100
        payloads = [packet(0x43, masks), display(0x06 if index == 0 else 0x38)]
        notifications = [dict(ordinal=ordinal, channel="display_short", bytesHex=payload,
                              kind="alert_row" if ordinal == 0 else "display_frame")
                         for ordinal, payload in enumerate(payloads)]
        stimuli.append(dict(schemaVersion=2, state="stimulus_requested", stimulusSequence=index + 1,
                            sourceIndex=index, replayOffsetSeconds=index, requestedHostMonotonicNs=start,
                            notifications=notifications, expected={"deliberately": "not an oracle"}))
        for ordinal, payload in enumerate(payloads):
            delivery.extend(emission(index * 2 + ordinal + 1, payload, start + ordinal * 10 + 1,
                                     start + ordinal * 10 + 5, index + 1, ordinal))
    return scenario, stimuli, delivery


def replace_planned(data, sequence, ordinal, payload):
    data[1][sequence - 1]["notifications"][ordinal]["bytesHex"] = payload
    for item in data[2]:
        if (item.get("stimulusSequence"), item.get("emissionOrdinal")) == (sequence, ordinal):
            item["payloadHex"] = payload
            item["payloadSha256"] = hashlib.sha256(bytes.fromhex(payload)).hexdigest()


class CounterExpectationTests(unittest.TestCase):
    def test_independent_digits_and_case_sensitive_modes(self):
        # Standard segment topology, independently enumerated visible meanings.
        for value, mask in enumerate((0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F)):
            with self.subTest(value=value):
                self.assertEqual(decode_counter_packet(display(mask))["fields"],
                                 {"count": {"allowed": [value]}, "mode": {"allowed": [None]}})
        for mask, value in ((0x77, "A"), (0x38, "L"), (0x18, "l")):
            self.assertEqual(decode_counter_packet(display(mask))["fields"],
                             {"count": {"allowed": [None]}, "mode": {"allowed": [value]}})

    def test_dot_and_blink_planes_preserve_allowed_phases(self):
        decoded = decode_counter_packet(display(0x86, 0x80))
        self.assertEqual(decoded["fields"]["count"], {"allowed": [1, None]})
        self.assertEqual(decoded["fields"]["mode"], {"allowed": [None]})
        self.assertEqual(decoded["permitted_pairs"], [{"count": 1, "mode": None}, {"count": None, "mode": None}])
        self.assertEqual(decode_counter_packet(display(0x86, 6))["fields"]["count"], {"allowed": [1]})

    def test_unknown_glyph_does_not_assert_absence(self):
        for mask in (0x50, 0x39):
            self.assertTrue(all("unresolved" in value for value in decode_counter_packet(display(mask))["fields"].values()))

    def test_mixed_count_mode_planes_do_not_create_impossible_independent_pairs(self):
        decoded = decode_counter_packet(display(6, 0x38))
        self.assertEqual(decoded["permitted_pairs"], [{"count": 1, "mode": None}, {"count": None, "mode": "L"}])
        self.assertTrue(all("unresolved" in value for value in decoded["fields"].values()))

    def test_packet_corruptions_rejected(self):
        good = bytearray.fromhex(display(6))
        bad_checksum = good.copy(); bad_checksum[-2] ^= 1
        bad_length = good.copy(); bad_length[4] += 1
        bad_end = good.copy(); bad_end[-1] = 0
        for value in ("", "nothex", "aa 00", bad_checksum.hex(), bad_length.hex(), bad_end.hex(),
                      packet(0x31, [6] * 7), packet(0x43, [6] * 8),
                      packet(0x31, [6] * 8, origin=0xE1), packet(0x31, [6] * 8, destination=0xB8)):
            with self.subTest(value=value), self.assertRaises(CounterEvidenceError):
                decode_counter_packet(value)

    def test_wire_result_ignores_expected_payload_and_preserves_inputs(self):
        data = fixture(); before = copy.deepcopy(data)
        result = counter_expectation_at(build_counter_timeline(*data), 150)
        self.assertEqual(result["fields"], {"count": {"allowed": [1]}, "mode": {"allowed": [None]}})
        self.assertEqual(result["input"]["display_requested_ns"], 111)
        self.assertEqual(result["input"]["display_accepted_ns"], 115)
        self.assertIn("CoreBluetooth acceptance does not establish DUT receipt.", result["comparison_basis"])
        self.assertEqual(data, before)

    def test_idle_requires_explicit_configuration_but_no_persistence_delay(self):
        timeline = build_counter_timeline(*fixture())
        for setting in (None, True):
            self.assertIn("unresolved", counter_expectation_at(timeline, 215, stealth_enabled=setting)["fields"]["mode"])
        result = counter_expectation_at(timeline, 215, stealth_enabled=False)
        self.assertEqual(result["fields"], {"count": {"allowed": [None]}, "mode": {"allowed": ["L"]}})
        with self.assertRaises(CounterEvidenceError):
            counter_expectation_at(timeline, 215, stealth_enabled=0)

    def test_capture_before_input_or_during_delivery_is_unresolved(self):
        timeline = build_counter_timeline(*fixture())
        for timestamp in (0, 99, 100, 114, 200, 214):
            with self.subTest(timestamp=timestamp):
                self.assertIn("unresolved", counter_expectation_at(timeline, timestamp, stealth_enabled=False)["fields"]["count"])
        self.assertEqual(counter_expectation_at(timeline, 115)["fields"]["count"], {"allowed": [1]})

    def test_delayed_backpressure_is_valid(self):
        data = fixture()
        retry = dict(data[2][0], state="notification_delayed", hostMonotonicNs=103, attemptedHostMonotonicNs=102)
        data[2].insert(1, retry)
        self.assertEqual(counter_expectation_at(build_counter_timeline(*data), 150)["fields"]["count"], {"allowed": [1]})

    def test_missing_duplicate_and_terminal_loss_rejected(self):
        for mutation in (lambda d: d[2].pop(), lambda d: d[2].append(copy.deepcopy(d[2][-1])),
                         lambda d: d[2].insert(1, copy.deepcopy(d[2][0])),
                         lambda d: d[2][-1].update(state="notification_dropped")):
            data = fixture(); mutation(data)
            with self.assertRaises(CounterEvidenceError):
                build_counter_timeline(*data)

    def test_payload_hash_pairing_timestamp_and_ordinal_corruptions_rejected(self):
        for mutation in (lambda d: d[2][1].update(payloadSha256="0" * 64),
                         lambda d: d[2][1].update(payloadHex=display(0x5B)),
                         lambda d: d[2][1].update(attemptedHostMonotonicNs=90),
                         lambda d: d[2][1].update(emissionOrdinal=8),
                         lambda d: d[2][1].update(globalTxSequence=True),
                         lambda d: d[1][0]["notifications"][0].update(ordinal=4)):
            data = fixture(); mutation(data)
            with self.assertRaises(CounterEvidenceError):
                build_counter_timeline(*data)

    def test_coherently_rehashed_wire_semantic_corruptions_rejected(self):
        for payload, ordinal in ((display(0x5B), 1),
                                 (packet(0x43, [0x21, 0x5E, 0x56, 1, 0, 0x24, 0x80]), 0),
                                 (packet(0x43, [0x11, 0x5E, 0x57, 1, 0, 0x24, 0x80]), 0),
                                 (packet(0x43, [0x11, 0x5E, 0x56, 1, 0, 0x44, 0x80]), 0)):
            data = fixture(); replace_planned(data, 1, ordinal, payload)
            with self.assertRaises(CounterEvidenceError):
                build_counter_timeline(*data)

    def test_scenario_swap_is_cross_checked_with_wire_not_expected(self):
        for mutation in (lambda d: d[0]["samples"][0].update(alerts=[]),
                         lambda d: d[0]["samples"][0].update(offsetSeconds=0.5),
                         lambda d: d[0]["samples"][0]["alerts"][0].update(frequencyMHz=24125),
                         lambda d: d[0]["samples"].pop()):
            data = fixture(); mutation(data)
            with self.assertRaises(CounterEvidenceError):
                build_counter_timeline(*data)

    def test_malformed_notification_and_boolean_ordinal_are_evidence_errors(self):
        for mutation in (lambda d: d[1][0].update(notifications=[None]),
                         lambda d: d[1][0]["notifications"][1].update(ordinal=True),
                         lambda d: d[0].update(schemaVersion=True)):
            data = fixture(); mutation(data)
            with self.assertRaises(CounterEvidenceError):
                build_counter_timeline(*data)

    def test_unscoped_display_is_not_ignored(self):
        data = fixture(); data[2].extend(emission(5, display(0x77), 220, 225))
        result = counter_expectation_at(build_counter_timeline(*data), 230, stealth_enabled=False)
        self.assertEqual(result["fields"]["mode"], {"allowed": ["A"]})
        self.assertIsNone(result["input"]["display_stimulus_sequence"])
        self.assertEqual(result["input"]["global_tx_sequence"], 5)

    def test_unscoped_row_invalidates_scenario_context(self):
        data = fixture(); data[2].extend(emission(5, packet(0x43, [0] * 7), 220, 225))
        result = counter_expectation_at(build_counter_timeline(*data), 230, stealth_enabled=False)
        self.assertIn("unresolved", result["fields"]["mode"])

    def test_unscoped_numeric_disagreement_is_unknown(self):
        data = fixture(); data[2].extend(emission(5, display(0x5B), 120, 125))
        result = counter_expectation_at(build_counter_timeline(*data), 150)
        self.assertIn("unresolved", result["fields"]["count"])

    def test_pending_unscoped_display_has_no_invented_response_deadline(self):
        data = fixture(); data[2].extend(emission(5, display(0x77), 220, 225))
        result = counter_expectation_at(build_counter_timeline(*data), 223, stealth_enabled=False)
        self.assertIn("unresolved", result["fields"]["mode"])

    def test_live_mode_glyph_is_outside_simple_count_rule(self):
        data = fixture(); replace_planned(data, 1, 1, display(0x77))
        result = counter_expectation_at(build_counter_timeline(*data), 150)
        self.assertIn("unresolved", result["fields"]["mode"])


if __name__ == "__main__":
    unittest.main()
