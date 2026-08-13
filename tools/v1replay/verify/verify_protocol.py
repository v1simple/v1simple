#!/usr/bin/env python3
"""
Cross-check v1replay's packet builders against the firmware that will parse them.

This is a port of two things that must agree:

  * the builders in Sources/v1replay/V1Protocol.swift
  * the parser in v1simple/src/packet_parser.cpp and packet_parser_alerts.cpp

It generates a complete matrix of bands, directions, strengths, and mute states,
runs every packet through a Python transcription of the firmware's parser, and
asserts that values survive the round trip. It reads no replay or capture files.

Run:  python3 verify/verify_protocol.py
"""

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

# ---------------------------------------------------------------------------
# Port of Sources/v1replay/V1Protocol.swift
# ---------------------------------------------------------------------------

PACKET_START = 0xAA
PACKET_END = 0xAB

HEADER_V1_TO_APP = (0xD6, 0xEA)
HEADER_BROADCAST_INFORMATION = (0xD8, 0xEA)
HEADER_DRAFT = (0xDA, 0xE4)

BANDS = {"laser": 0x01, "ka": 0x02, "k": 0x04, "x": 0x08, "ku": 0x10}
DIRECTIONS = {"F": 0x20, "S": 0x40, "R": 0x80}

AUX0_SOFT_MUTE = 0x01
AUX0_SYSTEM_STATUS = 0x04
AUX0_DISPLAY_ON = 0x08
MUTE_BIT = 0x10

BOGEY_GLYPHS = {0: 0x3F, 1: 0x06, 2: 0x5B, 3: 0x4F, 4: 0x66,
                5: 0x6D, 6: 0x7D, 7: 0x07, 8: 0x7F, 9: 0x6F}

MODE_ADVANCED_LOGIC = 0x38
MODE_BITS_ADVANCED_LOGIC = 0x0C

RAW_STRENGTH = {
    0x02: [0x00, 0x80, 0x93, 0x9A, 0xA1, 0xA8, 0xAF, 0xB6, 0xBD],  # ka
    0x08: [0x00, 0x80, 0x9B, 0xA5, 0xAF, 0xB8, 0xC1, 0xCA, 0xD8],  # x
    0x04: [0x00, 0x80, 0x8C, 0x95, 0x9F, 0xA9, 0xB3, 0xBD, 0xC6],  # k
    0x10: [0x00, 0x80, 0x8C, 0x95, 0x9F, 0xA9, 0xB3, 0xBD, 0xC6],  # ku
}


def led_bitmap(bars):
    bars = max(0, min(8, bars))
    return ((1 << bars) - 1) & 0xFF


def raw_strength(bars, band_mask):
    n = max(0, min(8, bars))
    if n == 0:
        return 0x00
    if band_mask == BANDS["laser"]:
        return 0xFF
    return RAW_STRENGTH[band_mask][n]


def frame(header, packet_id, payload, checksum=True):
    packet = [PACKET_START, header[0], header[1], packet_id,
              (len(payload) + (1 if checksum else 0)) & 0xFF]
    packet += list(payload)
    if checksum:
        packet.append(sum(packet) & 0xFF)
    packet.append(PACKET_END)
    return packet


def display_payload_alerting(bars, band_mask, direction_mask, bogey_count,
                             muted, volume, display_on, blink_plane=False,
                             blink_arrow=False,
                             aux1_mode_bits=MODE_BITS_ADVANCED_LOGIC):
    glyph = BOGEY_GLYPHS[min(9, max(0, bogey_count))]
    image = band_mask | direction_mask
    if muted:
        image |= MUTE_BIT
    aux0 = AUX0_SYSTEM_STATUS
    if display_on:
        aux0 |= AUX0_DISPLAY_ON
    if muted:
        aux0 |= AUX0_SOFT_MUTE
    image2 = image & ~direction_mask if blink_arrow else image
    return [glyph, 0x00 if blink_plane else glyph,
            led_bitmap(bars), image, image2, aux0, aux1_mode_bits, volume]


def display_payload_idle(mode_glyph, volume, display_on, soft_muted,
                         aux1_mode_bits=MODE_BITS_ADVANCED_LOGIC):
    aux0 = AUX0_SYSTEM_STATUS
    if display_on:
        aux0 |= AUX0_DISPLAY_ON
    if soft_muted:
        aux0 |= AUX0_SOFT_MUTE
    return [mode_glyph, mode_glyph, 0x00, 0x00, 0x00,
            aux0, aux1_mode_bits, volume]


def alert_row_payload(index, count, bars, band_mask, direction_mask,
                      frequency_mhz, priority, junk=False, photo_type=0):
    assert 1 <= count <= 15, "alert table count must be 1...15"
    assert 1 <= index <= count, "alert table index must be 1...count"
    raw = raw_strength(bars, band_mask)
    aux0 = photo_type & 0x0F
    if priority:
        aux0 |= 0x80
    if junk:
        aux0 |= 0x40
    rear = direction_mask == DIRECTIONS["R"]
    return [
        (index << 4) | count,
        (frequency_mhz >> 8) & 0xFF,
        frequency_mhz & 0xFF,
        0x00 if rear else raw,
        raw if rear else 0x00,
        band_mask | direction_mask,
        aux0,
    ]


def alert_payload(bars, band_mask, direction_mask, frequency_mhz,
                  priority=True, junk=False, photo_type=0):
    return alert_row_payload(1, 1, bars, band_mask, direction_mask,
                             frequency_mhz, priority, junk, photo_type)


def version_payload(version):
    digits = "".join(c for c in version if c.isdigit() or c == ".")
    payload = [ord("v")] + [ord(c) for c in digits]
    while len(payload) < 7:
        payload.append(ord("0"))
    return payload[:7]


def drain_frames(buffer):
    """Port of V1.drainFrames — returns (packets, leftover_buffer)."""
    packets = []
    buf = list(buffer)
    while True:
        if PACKET_START not in buf:
            buf = []
            break
        start = buf.index(PACKET_START)
        if start:
            buf = buf[start:]
        if len(buf) < 6:
            break
        declared = buf[4]
        total = 6 + declared
        if total < 6 or total > 64:
            buf = buf[1:]
            continue
        if len(buf) < total:
            break
        if buf[total - 1] != PACKET_END:
            buf = buf[1:]
            continue
        raw = buf[:total]
        payload_end = total - 2
        payload = raw[5:payload_end] if payload_end > 5 else []
        packets.append({"id": raw[3], "payload": payload, "raw": raw})
        buf = buf[total:]
    return packets, buf


# ---------------------------------------------------------------------------
# Port of v1simple/src/packet_parser*.cpp — the consumer side
# ---------------------------------------------------------------------------

def parser_validate(data):
    """PacketParser::validatePacket + the length/frame guards in parseInternal."""
    if len(data) < 7:
        return False, "packet shorter than 7 bytes"
    if data[0] != PACKET_START or data[-1] != PACKET_END:
        return False, "bad framing"
    if data[3] in (0x31, 0x43) and len(data) < 8:
        return False, "validatePacket requires >= 8 bytes"
    return True, ""


def parser_payload(data):
    """parseInternal: payload starts at [5], length = len - 6."""
    return data[5:len(data) - 1] if len(data) > 6 else []


def parse_display(payload):
    """PacketParser::parseDisplayData."""
    assert len(payload) >= 8, "display payload must be >= 8 bytes"
    image1 = payload[3]
    image2 = payload[4]
    aux0 = payload[5]
    system_status = (aux0 & 0x04) != 0

    bands = 0
    arrows = 0
    if system_status:
        if image1 & 0x01:
            bands |= BANDS["laser"]
        if image1 & 0x02:
            bands |= BANDS["ka"]
        if image1 & 0x04:
            bands |= BANDS["k"]
        if image1 & 0x08:
            bands |= BANDS["x"]
        if image1 & 0x20:
            arrows |= DIRECTIONS["F"]
        if image1 & 0x40:
            arrows |= DIRECTIONS["S"]
        if image1 & 0x80:
            arrows |= DIRECTIONS["R"]

    return {
        "signalBars": bin(payload[2]).count("1"),
        "ledBitmap": payload[2],
        "activeBands": bands,
        "arrows": arrows,
        "bandFlashBits": (image1 & ~image2) & 0x0F,
        "arrowFlashBits": (image1 & ~image2) & 0xE0,
        "rawMuteBit": (image1 & MUTE_BIT) != 0,
        "softMuted": (aux0 & 0x01) != 0,
        "systemStatus": system_status,
        "bogeyByte": payload[0],
        "volumeByte": payload[7],
    }


def map_strength_to_bars(band_mask, raw):
    """PacketParser::mapStrengthToBars."""
    if band_mask == BANDS["laser"]:
        return 8
    if band_mask == BANDS["ka"]:
        thresholds = [(0xBA, 8), (0xB3, 7), (0xAC, 6), (0xA5, 5),
                      (0x9E, 4), (0x97, 3), (0x90, 2), (0x01, 1)]
    elif band_mask == BANDS["x"]:
        thresholds = [(0xD0, 8), (0xC5, 7), (0xBD, 6), (0xB4, 5),
                      (0xAA, 4), (0xA0, 3), (0x96, 2), (0x01, 1)]
    elif band_mask in (BANDS["k"], BANDS["ku"]):
        thresholds = [(0xC2, 8), (0xB8, 7), (0xAE, 6), (0xA4, 5),
                      (0x9A, 4), (0x90, 3), (0x88, 2), (0x01, 1)]
    else:
        return 0
    for limit, bars in thresholds:
        if raw >= limit:
            return bars
    return 0


def decode_band(band_arrow):
    """PacketParser::decodeBand."""
    if band_arrow & 0x01:
        return BANDS["laser"]
    if band_arrow & 0x02:
        return BANDS["ka"]
    if band_arrow & 0x04:
        return BANDS["k"]
    if band_arrow & 0x08:
        return BANDS["x"]
    if (band_arrow & 0x1F) == 0x10:
        return BANDS["ku"]
    return 0


def decode_direction(band_arrow):
    if band_arrow & 0x20:
        return DIRECTIONS["F"]
    if band_arrow & 0x40:
        return DIRECTIONS["S"]
    if band_arrow & 0x80:
        return DIRECTIONS["R"]
    return 0


def parse_alert(payload):
    """PacketParser::parseAlertData, single-row case."""
    assert len(payload) >= 1
    index = (payload[0] >> 4) & 0x0F
    count = payload[0] & 0x0F
    if count == 0:
        return {"count": 0}

    one_based = 1 <= index <= count
    zero_based = index < count
    assert one_based or zero_based, "row index rejected by the firmware"

    chunk = (list(payload) + [0] * 8)[:8]
    band_arrow = chunk[5]
    aux0 = chunk[6]
    band = decode_band(band_arrow)
    return {
        "count": count,
        "index": index,
        "band": band,
        "direction": decode_direction(band_arrow),
        "frequency": 0 if band == BANDS["laser"] else (chunk[1] << 8) | chunk[2],
        "frontStrength": map_strength_to_bars(band, chunk[3]),
        "rearStrength": map_strength_to_bars(band, chunk[4]),
        "isPriority": (aux0 & 0x80) != 0,
        "isJunk": (aux0 & 0x40) != 0,
        "photoType": aux0 & 0x0F,
    }


# ---------------------------------------------------------------------------
# Checks
# ---------------------------------------------------------------------------

FAILURES = []
CHECKS = [0]


def check(condition, label, detail=""):
    CHECKS[0] += 1
    if not condition:
        FAILURES.append(label + ((" — " + detail) if detail else ""))


def hexs(values):
    return " ".join("%02X" % v for v in values)


def check_crib_packets():
    """The three packets from the LightBlue crib sheet, rebuilt by the tool."""
    alert = frame(HEADER_DRAFT, 0x43,
                  alert_payload(1, BANDS["ka"], DIRECTIONS["F"], 34700),
                  checksum=False)
    expected_alert = [0xAA, 0xDA, 0xE4, 0x43, 0x07, 0x11, 0x87, 0x8C,
                      0x80, 0x00, 0x22, 0x80, 0xAB]
    check(alert == expected_alert,
          "crib alert packet is byte-identical",
          "built %s / expected %s" % (hexs(alert), hexs(expected_alert)))

    draft_one = [0xAA, 0xDA, 0xE4, 0x31, 0x08, 0x06, 0x00, 0x01,
                 0x22, 0x22, 0x04, 0x00, 0x00, 0xAB]
    draft_six = [0xAA, 0xDA, 0xE4, 0x31, 0x08, 0x06, 0x00, 0x3F,
                 0x22, 0x22, 0x04, 0x00, 0x00, 0xAB]

    # --blink-bogey --no-checksum must reproduce the draft byte for byte.
    for bars, expected in ((1, draft_one), (6, draft_six)):
        built = frame(HEADER_DRAFT, 0x31,
                      display_payload_alerting(bars, BANDS["ka"], DIRECTIONS["F"], 1,
                                               False, 0x00, False, blink_plane=True,
                                               aux1_mode_bits=0x00),
                      checksum=False)
        check(built == expected,
              "--blink-bogey reproduces the draft %d-bar packet exactly" % bars,
              "built %s / draft %s" % (hexs(built), hexs(expected)))

    # The default differs in exactly one byte: the bogey blink plane.
    for bars, expected in ((1, draft_one), (6, draft_six)):
        built = frame(HEADER_DRAFT, 0x31,
                      display_payload_alerting(bars, BANDS["ka"], DIRECTIONS["F"], 1,
                                               False, 0x00, False,
                                               aux1_mode_bits=0x00),
                      checksum=False)
        differing = [i for i in range(len(expected)) if built[i] != expected[i]]
        check(differing == [6],
              "default differs from the draft in byte 6 only (%d bars)" % bars,
              "differs at %s" % differing)
        check(built[5] == built[6],
              "default sends a steady bogey glyph — image1 == image2, "
              "so the firmware's blink-refresh repaint stays off (%d bars)" % bars,
              "%02X vs %02X" % (built[5], built[6]))

    # And the draft packets themselves must still parse — they are what gets
    # pasted into LightBlue by hand.
    for packet, bars in ((draft_one, 1), (draft_six, 6)):
        ok, why = parser_validate(packet)
        check(ok, "draft display packet passes validatePacket", why)
        state = parse_display(parser_payload(packet))
        check(state["signalBars"] == bars,
              "draft display packet decodes to %d bars" % bars,
              "got %d" % state["signalBars"])
        check(state["activeBands"] == BANDS["ka"], "draft packet decodes Ka")
        check(state["arrows"] == DIRECTIONS["F"], "draft packet decodes front arrow")

    ok, why = parser_validate(expected_alert)
    check(ok, "draft alert packet passes validatePacket", why)
    alert_state = parse_alert(parser_payload(expected_alert))
    check(alert_state["frequency"] == 34700,
          "draft alert packet decodes synthetic frequency",
          str(alert_state["frequency"]))
    check(alert_state["isPriority"], "draft alert packet is flagged priority")


def check_repo_fixture_parity():
    """
    The repo already builds 15-byte display packets.

    test/test_protocol_spec_conformance/test_protocol_spec_conformance.cpp:69
    pushes 0xDA, 0xE4, then a 9-byte display payload
    {0x06, 0x00, bars, 0x24, 0x24, 0x04, 0x00, 0x00, 0x00} with
    len = payload.size(). That is eight payload bytes plus the checksum slot —
    structurally identical to the tool's fixture-compatible framing. The
    14-byte form in the hand-written crib is the outlier, not the house style.
    """
    fixture = [0xAA, 0xDA, 0xE4, 0x31, 0x09,
               0x06, 0x00, 0x3F, 0x24, 0x24, 0x04, 0x00, 0x00, 0x00, 0xAB]

    ok, why = parser_validate(fixture)
    check(ok, "repo conformance fixture validates", why)
    check(len(fixture) == 15, "repo conformance fixture is 15 bytes")
    check(fixture[4] == 0x09, "repo fixture declares a 9-byte payload region")

    state = parse_display(parser_payload(fixture))
    check(state["signalBars"] == 6, "repo fixture decodes six bars")
    check(state["activeBands"] == BANDS["k"], "repo fixture is a K-band row")

    built = frame(HEADER_DRAFT, 0x31,
                  display_payload_alerting(6, BANDS["k"], DIRECTIONS["F"], 1,
                                           False, 0x00, False, blink_plane=True,
                                           aux1_mode_bits=0x00),
                  checksum=True)
    check(len(built) == len(fixture),
          "fixture-compatible display packet is the same length as the repo fixture",
          "%d vs %d" % (len(built), len(fixture)))
    check(built[4] == fixture[4],
          "fixture-compatible length byte matches the repo fixture",
          "0x%02X vs 0x%02X" % (built[4], fixture[4]))
    check(built[:5] == fixture[:5],
          "fixture-compatible header and framing match the repo fixture",
          "%s vs %s" % (hexs(built[:5]), hexs(fixture[:5])))
    # Only the checksum slot should differ: the fixture parks 0x00 there, the
    # tool computes the real sum. Both are ignored by validatePacket.
    differing = [i for i in range(len(fixture)) if built[i] != fixture[i]]
    check(differing == [13],
          "fixture-compatible packet differs in the checksum slot only",
          "differs at %s" % differing)


def check_framing():
    """Length byte, checksum, and the parser's framing guards."""
    for checksum in (True, False):
        for header in (HEADER_BROADCAST_INFORMATION, HEADER_DRAFT):
            display = frame(header, 0x31,
                            display_payload_alerting(4, BANDS["ka"], DIRECTIONS["F"],
                                                     1, False, 0x40, True,
                                                     aux1_mode_bits=(
                                                         0x00 if header == HEADER_DRAFT
                                                         else MODE_BITS_ADVANCED_LOGIC
                                                     )),
                            checksum=checksum)
            expected_len = 8 + (1 if checksum else 0)
            check(display[4] == expected_len,
                  "display length byte counts payload + checksum",
                  "got 0x%02X want 0x%02X" % (display[4], expected_len))
            check(len(display) == 6 + display[4],
                  "total length == 6 + length byte")
            ok, why = parser_validate(display)
            check(ok, "built display packet passes validatePacket", why)

            payload = parser_payload(display)
            check(len(payload) >= 8,
                  "parser sees >= 8 display payload bytes",
                  "got %d" % len(payload))
            state = parse_display(payload)
            check(state["signalBars"] == 4, "round-trip bars")
            check(state["volumeByte"] == 0x40 if checksum else True,
                  "V4.1028+ full ID31 Aux2 carries current, not saved, volume",
                  "got 0x%02X" % state["volumeByte"])

            if checksum:
                body = display[:-2]
                check(display[-2] == sum(body) & 0xFF,
                      "checksum is the byte sum of everything before it")

    # A full V4.1028+ checksummed display packet is 15 bytes; the draft form is
    # 14 and pays for it by losing auxData2 to the checksum slot.
    spec = frame(HEADER_BROADCAST_INFORMATION, 0x31,
                 display_payload_alerting(1, BANDS["ka"], DIRECTIONS["F"], 1,
                                          False, 0x40, True), checksum=True)
    check(len(spec) == 15, "spec display packet is 15 bytes", "got %d" % len(spec))
    check(parse_display(parser_payload(spec))["volumeByte"] == 0x40,
          "V4.1028+ full ID31 reports current, not saved, volume 4/0")


def check_strength_round_trip():
    """Every bar count on every band must survive the alert-table RSSI mapping."""
    for name, mask in BANDS.items():
        if name == "laser":
            check(map_strength_to_bars(mask, raw_strength(8, mask)) == 8,
                  "laser maps to 8 bars")
            continue
        for bars in range(1, 9):
            raw = raw_strength(bars, mask)
            got = map_strength_to_bars(mask, raw)
            check(got == bars,
                  "%s: %d bars survives the RSSI round trip" % (name.upper(), bars),
                  "raw 0x%02X decoded to %d" % (raw, got))


def check_idle_frame():
    payload = display_payload_idle(MODE_ADVANCED_LOGIC, 0x40, True, False)
    packet = frame(HEADER_BROADCAST_INFORMATION, 0x31, payload)
    state = parse_display(parser_payload(packet))
    check(state["signalBars"] == 0, "idle frame shows no bars")
    check(state["activeBands"] == 0, "idle frame shows no band")
    check(state["arrows"] == 0, "idle frame shows no arrow")
    check(state["systemStatus"], "idle frame keeps isSystemStatus set")
    check(state["bogeyByte"] == MODE_ADVANCED_LOGIC,
          "idle frame carries the Advanced Logic mode glyph")
    check(payload[6] == MODE_BITS_ADVANCED_LOGIC,
          "idle frame carries Advanced Logic mode bits in auxData1")


def check_system_status_guard():
    """Without aux0 bit 2 the parser blanks bands and arrows — make sure we set it."""
    payload = display_payload_alerting(6, BANDS["ka"], DIRECTIONS["F"], 1,
                                       False, 0x40, True)
    check(payload[5] & AUX0_SYSTEM_STATUS,
          "alerting frames set isSystemStatus (or the firmware hides the alert)")

    broken = list(payload)
    broken[5] &= ~AUX0_SYSTEM_STATUS
    state = parse_display(broken)
    check(state["activeBands"] == 0 and state["arrows"] == 0,
          "clearing isSystemStatus does suppress bands/arrows (guard is real)")


def check_arrow_blink_planes():
    """The treatment clears only the selected direction bit from image2."""
    default_payload = display_payload_alerting(
        1, BANDS["ka"], DIRECTIONS["F"], 1, False, 0x40, True
    )
    check(default_payload == [0x06, 0x06, 0x01, 0x22, 0x22, 0x0C, 0x0C, 0x40],
          "default alerting payload remains byte-identical",
          hexs(default_payload))

    for direction_name, direction_mask in DIRECTIONS.items():
        for muted in (False, True):
            payload = display_payload_alerting(
                1, BANDS["ka"], direction_mask, 1, muted, 0x40, True,
                blink_arrow=True,
            )
            state = parse_display(payload)
            check(state["arrowFlashBits"] == direction_mask,
                  "arrow blink declares only the %s direction bit%s"
                  % (direction_name, " while muted" if muted else ""),
                  "image1=0x%02X image2=0x%02X"
                  % (payload[3], payload[4]))
            check(state["bandFlashBits"] == 0,
                  "arrow blink leaves band bits steady (%s)" % direction_name)
            check((payload[3] & 0x1F) == (payload[4] & 0x1F),
                  "arrow blink preserves band and mute bits (%s)" % direction_name)


def check_command_decoding():
    """Frames v1simple actually writes, per src/ble_commands.cpp."""
    def command(packet_id, payload):
        packet = [0xAA, 0xDA, 0xE6, packet_id, len(payload) + 1] + payload
        packet.append(sum(packet) & 0xFF)
        packet.append(0xAB)
        return packet

    req_version = command(0x01, [])
    req_all_volume = command(0x3C, [])
    mute_on = command(0x34, [])
    display_off = command(0x32, [0x00])
    write_volume = command(0x39, [4, 0, 0])
    start_alerts = command(0x41, [])

    stream = (req_version + req_all_volume + mute_on
              + display_off + write_volume + start_alerts)
    packets, leftover = drain_frames(stream)
    check(len(packets) == 6, "drainFrames finds all six commands",
          "found %d" % len(packets))
    check(leftover == [], "no leftover bytes")
    check([p["id"] for p in packets] == [0x01, 0x3C, 0x34, 0x32, 0x39, 0x41],
          "command IDs decode in order")
    check(packets[3]["payload"] == [0x00], "reqTurnOffMainDisplay carries its mode byte")
    check(packets[4]["payload"] == [4, 0, 0], "reqWriteVolume carries three bytes")

    # Fragmented delivery: BLE writes can split anywhere.
    for split in range(1, len(stream)):
        first, rest = drain_frames(stream[:split])
        more, tail = drain_frames(rest + stream[split:])
        ids = [p["id"] for p in first + more]
        if ids != [0x01, 0x3C, 0x34, 0x32, 0x39, 0x41] or tail != []:
            check(False, "fragmented stream reassembles at every split point",
                  "split %d produced %s" % (split, ids))
            break
    else:
        check(True, "fragmented stream reassembles at every split point")

    # Garbage in front of a real packet must resync, not wedge.
    noisy = [0x00, 0xFF, 0x12] + req_version
    packets, leftover = drain_frames(noisy)
    check([p["id"] for p in packets] == [0x01], "resyncs past leading garbage")
    check(leftover == [], "no leftover after resync")


def check_version_reply():
    payload = version_payload("4.1038")
    check(payload == [ord(c) for c in "v4.1038"],
          "respVersion payload is 'v4.1038'", str(payload))
    letter = payload[0]
    ok = (chr(letter) in "vV"
          and chr(payload[1]).isdigit() and payload[2] == ord(".")
          and all(chr(b).isdigit() for b in payload[3:7]))
    check(ok, "respVersion matches the parser's 7-byte shape test")
    version = (int(chr(payload[1])) * 10000 + int(chr(payload[3])) * 1000
               + int(chr(payload[4])) * 100 + int(chr(payload[5])) * 10
               + int(chr(payload[6])))
    check(version == 41038, "respVersion decodes to 41038", str(version))
    check(version >= 41032, "junk flag support is enabled at this version")
    check(version >= 41037, "photo type support is enabled at this version")


def check_replies():
    """Targeted replies and alert clears survive parser length guards."""
    for header in (HEADER_V1_TO_APP, HEADER_DRAFT):
        for checksum in (True, False):
            tag = "%s/%s" % ("spec" if header == HEADER_V1_TO_APP else "draft",
                             "checksum" if checksum else "no-checksum")

            # respVersion and respAllVolume are NOT on the parser's bypass list,
            # so they additionally have to clear validatePacket's 8-byte floor.
            version = frame(header, 0x02, version_payload("4.1038"), checksum=checksum)
            check(len(version) >= 8,
                  "respVersion clears validatePacket (%s)" % tag,
                  "%d bytes" % len(version))
            check(version[4] >= 7 and len(version) - 6 >= 7,
                  "respVersion declares and carries 7 payload bytes (%s)" % tag,
                  "len byte 0x%02X, payload %d" % (version[4], len(version) - 6))

            volume = frame(header, 0x3D, [4, 0, 4, 0], checksum=checksum)
            check(len(volume) >= 8,
                  "respAllVolume clears validatePacket (%s)" % tag,
                  "%d bytes" % len(volume))
            check(volume[4] >= 4 and len(volume) - 6 >= 4,
                  "respAllVolume declares and carries 4 payload bytes (%s)" % tag,
                  "len byte 0x%02X, payload %d" % (volume[4], len(volume) - 6))

    for header in (HEADER_BROADCAST_INFORMATION, HEADER_DRAFT):
        for checksum in (True, False):
            tag = "%s/%s" % ("broadcast" if header == HEADER_BROADCAST_INFORMATION else "draft",
                             "checksum" if checksum else "no-checksum")
            empty = frame(header, 0x43, [0] * 7, checksum=checksum)
            ok, why = parser_validate(empty)
            check(ok, "empty alert table clears validatePacket (%s)" % tag, why)
            check(parse_alert(parser_payload(empty))["count"] == 0,
                  "empty alert table decodes as count 0 (%s)" % tag)


def check_generated_alert_tables():
    """Complete one-based 0/1/2/3-row tables survive both frame forms."""
    table_cases = [
        [
            (1, 4, BANDS["k"], DIRECTIONS["F"], 24150, True),
        ],
        [
            (1, 5, BANDS["k"], DIRECTIONS["S"], 24150, False),
            (2, 6, BANDS["ka"], DIRECTIONS["F"], 34700, True),
        ],
        [
            (1, 8, BANDS["k"], DIRECTIONS["S"], 24150, False),
            (2, 6, BANDS["ka"], DIRECTIONS["F"], 34700, True),
            (3, 4, BANDS["ka"], DIRECTIONS["R"], 35500, False),
        ],
    ]

    for checksum in (True, False):
        frame_tag = "checksum" if checksum else "no-checksum"

        empty_packet = frame(HEADER_DRAFT, 0x43, [0] * 7, checksum=checksum)
        ok, why = parser_validate(empty_packet)
        check(ok, "empty alert table validates (%s)" % frame_tag, why)
        empty_payload = parser_payload(empty_packet)
        check(empty_payload[:7] == [0] * 7,
              "empty alert table carries seven zero bytes (%s)" % frame_tag,
              hexs(empty_payload[:7]))
        check(parse_alert(empty_payload)["count"] == 0,
              "explicit empty table clears published rows (%s)" % frame_tag)

        for expected_count, specs in enumerate(table_cases, start=1):
            decoded = []
            for index, bars, band, direction, frequency, priority in specs:
                payload = alert_row_payload(index, expected_count, bars, band,
                                            direction, frequency, priority)
                packet = frame(HEADER_DRAFT, 0x43, payload, checksum=checksum)
                ok, why = parser_validate(packet)
                check(ok,
                      "%d-row alert packet %d validates (%s)"
                      % (expected_count, index, frame_tag), why)
                decoded.append(parse_alert(parser_payload(packet)))

            check([row["index"] for row in decoded] == list(range(1, expected_count + 1)),
                  "%d-row table preserves one-based row order (%s)"
                  % (expected_count, frame_tag),
                  str([row["index"] for row in decoded]))
            check(all(row["count"] == expected_count for row in decoded),
                  "%d-row table repeats the complete count (%s)"
                  % (expected_count, frame_tag))
            check(len(decoded) == expected_count,
                  "%d-row table is complete (%s)" % (expected_count, frame_tag))
            check(sum(1 for row in decoded if row["isPriority"]) == 1,
                  "%d-row table has exactly one priority (%s)"
                  % (expected_count, frame_tag))

            for row, spec in zip(decoded, specs):
                index, bars, band, direction, frequency, priority = spec
                selected_strength = (row["rearStrength"]
                                     if direction == DIRECTIONS["R"]
                                     else row["frontStrength"])
                check(row["index"] == index
                      and row["frequency"] == frequency
                      and row["band"] == band
                      and row["direction"] == direction
                      and selected_strength == bars
                      and row["isPriority"] == priority,
                      "%d-row table row %d round-trips (%s)"
                      % (expected_count, index, frame_tag),
                      str(row))

            if expected_count >= 2:
                check(decoded[1]["isPriority"],
                      "%d-row table selects row 2 by its priority flag (%s)"
                      % (expected_count, frame_tag))


def check_generated_matrix():
    """Generated protocol coverage with no fixture or capture dependency."""
    frequencies = {
        "ka": 34700,
        "k": 24150,
        "x": 10525,
        "ku": 13450,
        "laser": 0,
    }
    cases = 0
    for band_name, band_mask in BANDS.items():
        for direction_mask in DIRECTIONS.values():
            for bars in range(1, 9):
                for muted in (False, True):
                    cases += 1
                    display = frame(
                        HEADER_BROADCAST_INFORMATION,
                        0x31,
                        display_payload_alerting(
                            bars, band_mask, direction_mask, 1, muted, 0x40, True
                        ),
                    )
                    ok, why = parser_validate(display)
                    if not ok:
                        check(False, "generated display packet validates", why)
                        return

                    state = parse_display(parser_payload(display))
                    # The display image has no distinct Ku decode path in the
                    # owning parser; bit 0x10 is the mute bit. Ku remains
                    # available through alert rows, so display reports no band.
                    expected_display_band = 0 if band_name == "ku" else band_mask
                    expected_raw_mute = muted or band_name == "ku"
                    check(
                        state["signalBars"] == bars
                        and state["ledBitmap"] == led_bitmap(bars)
                        and state["activeBands"] == expected_display_band
                        and state["arrows"] == direction_mask
                        and state["rawMuteBit"] == expected_raw_mute
                        and state["bandFlashBits"] == 0
                        and state["arrowFlashBits"] == 0,
                        "generated display matrix round-trips",
                        "%s/%02X/%d/%s" % (band_name, direction_mask, bars, muted),
                    )

                    alert = frame(
                        HEADER_BROADCAST_INFORMATION,
                        0x43,
                        alert_payload(
                            bars, band_mask, direction_mask, frequencies[band_name]
                        ),
                    )
                    ok, why = parser_validate(alert)
                    if not ok:
                        check(False, "generated alert packet validates", why)
                        return

                    row = parse_alert(parser_payload(alert))
                    strength = (
                        row["rearStrength"]
                        if direction_mask == DIRECTIONS["R"]
                        else row["frontStrength"]
                    )
                    # Laser is presence-only in the parser and always maps to
                    # the full eight-bar state regardless of its raw byte.
                    expected_alert_strength = 8 if band_name == "laser" else bars
                    check(
                        row["count"] == 1
                        and row["band"] == band_mask
                        and row["direction"] == direction_mask
                        and row["frequency"] == frequencies[band_name]
                        and strength == expected_alert_strength
                        and row["isPriority"],
                        "generated alert matrix round-trips",
                        "%s/%02X/%d" % (band_name, direction_mask, bars),
                    )
    print("    %d generated cases" % cases)


def check_swift_python_parity():
    """Guard against the two ports drifting: the tables must appear in both."""
    here = os.path.dirname(os.path.abspath(__file__))
    swift_path = os.path.join(here, "..", "Sources", "v1replay", "V1Protocol.swift")
    try:
        with open(swift_path) as handle:
            swift = handle.read()
    except OSError:
        check(False, "V1Protocol.swift is readable", swift_path)
        return

    for band, table in RAW_STRENGTH.items():
        if band == BANDS["ku"]:
            continue
        needle = ", ".join("0x%02X" % v for v in table)
        check(needle in swift,
              "Swift carries the same RSSI table as this checker",
              "missing: [%s]" % needle)

    for count, glyph in BOGEY_GLYPHS.items():
        if count > 8:
            continue
        check(("case %d: return 0x%02X" % (count, glyph)) in swift,
              "Swift carries the same bogey glyph for %d" % count)

    check("static func row(index: Int," in swift,
          "Swift exposes the general alert-row builder")
    check('precondition(count >= 1 && count <= 15, "alert table count must be 1...15")' in swift,
          "Swift enforces the alert-table count domain")
    check('precondition(index >= 1 && index <= count, "alert table index must be 1...count")' in swift,
          "Swift enforces one-based alert-table indexes")
    check("return row(index: 1," in swift,
          "Swift single-row builder delegates to the general builder")
    check("return AlertRow(index: 0, count: 0, frequencyMHz: 0," in swift,
          "Swift keeps the explicit seven-zero-byte empty row")


BENCH_HARNESS = r'''
import Foundation

private var failures: [String] = []
private var checks = 0

private func check(_ condition: @autoclosure () -> Bool, _ label: String) {
    checks += 1
    if !condition() { failures.append(label) }
}

private func fields(_ line: Substring) -> [Substring] {
    return line.split(separator: ",", omittingEmptySubsequences: false)
}

@main
struct BenchVerifier {
    static func main() {
        var userBytes = V1.UserBytesStore()
        check(userBytes.bytes == [0, 0, 0, 0, 0, 0],
              "user-byte store starts with six zero bytes")
        check(userBytes.write([1, 2, 3, 4, 5, 6]),
              "user-byte store accepts an exact six-byte write")
        check(userBytes.bytes == [1, 2, 3, 4, 5, 6],
              "user-byte readback returns the last accepted write")
        check(!userBytes.write([9, 9, 9]),
              "user-byte store rejects a malformed write")
        check(userBytes.bytes == [1, 2, 3, 4, 5, 6],
              "malformed write does not corrupt stored user bytes")

        let encounter = BenchScenario.make()
        let csv = BenchScenario.expectedCSV(for: encounter)
        let lines = csv.split(separator: "\n", omittingEmptySubsequences: false)
        let header = "offset_s,phase,active_alert_count,priority_frequency_mhz,priority_band,priority_direction,priority_bars,scenario_arrow_blink,card_1_frequency_mhz,card_1_direction,card_1_bars,card_2_frequency_mhz,card_2_direction,card_2_bars"

        check(csv == BenchScenario.expectedCSV(for: BenchScenario.make()),
              "two generated exports are byte-identical")
        check(lines.first == Substring(header), "CSV has the exact 14-column header")
        check(lines.count == 763, "CSV has one header and 762 timeline rows")
        check(encounter.samples.count == 762, "scenario has 762 samples")
        check(encounter.samples.filter { !$0.alerts.isEmpty }.count == 708,
              "scenario has 708 active table publishes")
        check(encounter.samples.filter { $0.alerts.count == 3 }.count == 30,
              "scenario has 30 three-bogey publishes")
        check(encounter.samples.filter(\.scenarioArrowBlink).count == 57,
              "scenario has 57 priority-arrow blink samples")
        check(ArrowBlinkProfile.scenario.sampleCount(in: encounter) == 57,
              "scenario profile exposes the authored 19-second blink window")
        check(ArrowBlinkProfile.steady.sampleCount(in: encounter) == 0,
              "steady control has no arrow blink samples")
        check(ArrowBlinkProfile.stress.sampleCount(in: encounter) == 708,
              "stress control blinks every active priority-arrow sample")

        let expectedPhases = [
            "idle": 15, "k_encounter": 36, "ka_encounter": 36,
            "priority_handoff": 30, "three_bogeys": 30,
            "handoff_clear": 30, "duke_shaped_approach": 555,
            "idle_tail": 30,
        ]
        let phaseCounts = Dictionary(grouping: encounter.samples, by: { $0.phase })
            .mapValues(\.count)
        check(phaseCounts == expectedPhases, "phase boundaries have expected sample counts")

        for (index, sample) in encounter.samples.enumerated() {
            check(abs(sample.offset - Double(index) / 3.0) < 0.000_001,
                  "sample \(index) is on the nominal 3 Hz cadence")
            check(fields(lines[index + 1]).count == 14,
                  "CSV row \(index) has 14 columns")
        }

        check(encounter.samples[..<(33 * 3)].allSatisfy { !$0.scenarioArrowBlink },
              "scenario is steady before the multi-alert interval")
        check(encounter.samples[(33 * 3)..<(52 * 3)].allSatisfy(\.scenarioArrowBlink),
              "scenario blinks throughout the authored multi-alert interval")
        check(encounter.samples[(52 * 3)...].allSatisfy { !$0.scenarioArrowBlink },
              "scenario returns to steady for the remaining single-alert interval")

        let handoff = encounter.samples[33 * 3]
        check(handoff.alerts.count == 2, "handoff has two complete rows")
        check(!handoff.alerts[0].isPriority && handoff.alerts[1].isPriority,
              "handoff selects row 2 with the priority flag")
        check(handoff.priorityAlert?.frequencyMHz == 34_700,
              "row-2 Ka alert drives the primary display")

        let three = encounter.samples[39 * 3]
        check(three.alerts.map(\.frequencyMHz) == [24_150, 34_700, 35_500],
              "three-bogey table preserves established row order")
        let threeCSV = fields(lines[39 * 3 + 1])
        check(threeCSV[7] == "1", "three-bogey CSV marks the scenario arrow blink")
        check(threeCSV[8] == "24150" && threeCSV[9] == "SIDE" && threeCSV[10] == "3",
              "card 1 has K frequency, direction, and projected bars")
        check(threeCSV[11] == "35500" && threeCSV[12] == "REAR" && threeCSV[13] == "3",
              "card 2 has Ka frequency, direction, and projected bars")

        let dukeStart = 59 * 3
        let first95 = encounter.samples[dukeStart..<(dukeStart + 95 * 3)]
        check(first95.filter { $0.priorityAlert?.strength == 1 }.count > first95.count * 9 / 10,
              "first 95 Duke-shaped seconds are mostly one bar")
        check(first95.contains { $0.priorityAlert?.strength == 2 },
              "first 95 Duke-shaped seconds include occasional two-bar samples")
        let plateau = encounter.samples[(dukeStart + 120 * 3)..<(dukeStart + 140 * 3)]
        check(plateau.count == 60 && plateau.allSatisfy { $0.priorityAlert?.strength == 6 },
              "Duke-shaped approach has an exact 20-second six-bar plateau")
        check(encounter.samples[(244 * 3)...].allSatisfy { $0.alerts.isEmpty },
              "scenario ends with a clean 10-second idle tail")

        let steadyFront = V1.DisplayFrame.alerting(
            bars: 1, band: .ka, direction: .front, bogeyCount: 1,
            muted: false, volume: 0x40, displayOn: true
        )
        check(steadyFront.payload == [0x06, 0x06, 0x01, 0x22, 0x22, 0x0C, 0x0C, 0x40],
              "default Swift alerting payload remains byte-identical")

        let directionCases: [(V1.Direction, UInt8)] = [
            (.front, 0x20), (.side, 0x40), (.rear, 0x80),
        ]
        for (direction, directionBit) in directionCases {
            for muted in [false, true] {
                let frame = V1.DisplayFrame.alerting(
                    bars: 1, band: .ka, direction: direction, bogeyCount: 1,
                    muted: muted, volume: 0x40, displayOn: true,
                    blinkArrow: true
                )
                let image1 = frame.payload[3]
                let image2 = frame.payload[4]
                check((image1 & ~image2) == directionBit,
                      "Swift blink frame clears only its direction bit")
                check((image1 & 0x1F) == (image2 & 0x1F),
                      "Swift blink frame preserves band and mute bits")
            }
        }

        let blinkingFront = V1.DisplayFrame.alerting(
            bars: 1, band: .ka, direction: .front, bogeyCount: 1,
            muted: false, volume: 0x40, displayOn: true,
            blinkArrow: true
        )
        check(blinkingFront.payload[3] == 0x22 && blinkingFront.payload[4] == 0x02,
              "Swift front blink emits image1=22 image2=02")

        if failures.isEmpty {
            print("\(checks) Swift bench checks")
            return
        }
        for failure in failures { print("  - \(failure)") }
        exit(1)
    }
}
'''


def check_generated_swift_bench():
    """Compile the portable Swift model and validate its actual CSV output."""
    source_root = Path(__file__).resolve().parents[1] / "Sources" / "v1replay"
    try:
        with tempfile.TemporaryDirectory(prefix="v1replay-bench-") as temporary:
            temporary_path = Path(temporary)
            harness = temporary_path / "BenchVerifier.swift"
            binary = temporary_path / "bench-verifier"
            module_cache = temporary_path / "module-cache"
            module_cache.mkdir()
            harness.write_text(BENCH_HARNESS, encoding="utf-8")

            if sys.platform == "darwin" and shutil.which("xcrun"):
                compiler = ["xcrun", "swiftc"]
            elif shutil.which("swiftc"):
                compiler = [shutil.which("swiftc")]
            else:
                raise RuntimeError("swiftc is required")

            command = compiler + [
                "-module-cache-path", str(module_cache),
                str(source_root / "V1Protocol.swift"),
                str(source_root / "Encounter.swift"),
                str(source_root / "BenchScenario.swift"),
                str(harness),
                "-o", str(binary),
            ]
            environment = os.environ.copy()
            environment["CLANG_MODULE_CACHE_PATH"] = str(module_cache)
            developer = Path("/Applications/Xcode.app/Contents/Developer")
            if sys.platform == "darwin" and developer.is_dir():
                environment["DEVELOPER_DIR"] = str(developer)

            compiled = subprocess.run(command, text=True, capture_output=True,
                                      env=environment, check=False)
            check(compiled.returncode == 0, "portable Swift bench model compiles",
                  (compiled.stdout + compiled.stderr).strip())
            if compiled.returncode != 0:
                return

            verified = subprocess.run([str(binary)], text=True, capture_output=True,
                                      check=False)
            check(verified.returncode == 0,
                  "actual Swift bench timeline and expected CSV pass",
                  (verified.stdout + verified.stderr).strip())
            if verified.returncode == 0:
                print("    " + verified.stdout.strip())
    except (OSError, RuntimeError) as error:
        check(False, "portable Swift bench verification runs", str(error))


def main():
    print("v1replay protocol verification")
    print("  crib sheet packets")
    check_crib_packets()
    print("  parity with the repo's own test fixtures")
    check_repo_fixture_parity()
    print("  framing, length byte, checksum")
    check_framing()
    print("  RSSI ↔ bar round trip")
    check_strength_round_trip()
    print("  idle frame")
    check_idle_frame()
    print("  isSystemStatus guard")
    check_system_status_guard()
    print("  arrow blink planes")
    check_arrow_blink_planes()
    print("  inbound command decoding")
    check_command_decoding()
    print("  respVersion")
    check_version_reply()
    print("  reply and alert-clear length floors")
    check_replies()
    print("  complete 0/1/2/3-row alert tables")
    check_generated_alert_tables()
    print("  Swift/Python table parity")
    check_swift_python_parity()
    print("  generated Swift bench timeline and expected CSV")
    check_generated_swift_bench()
    print("  generated protocol matrix")
    check_generated_matrix()

    print()
    if FAILURES:
        print("FAILED — %d of %d checks" % (len(FAILURES), CHECKS[0]))
        for failure in FAILURES:
            print("  ✗ " + failure)
        return 1

    print("OK — %d checks passed" % CHECKS[0])
    return 0


if __name__ == "__main__":
    sys.exit(main())
