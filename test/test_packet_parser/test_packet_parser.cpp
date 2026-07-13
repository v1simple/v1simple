#include <unity.h>

#include <cstdio>
#include "../mocks/Arduino.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

// packet_parser.cpp pulls ../include/config.h. In native tests we only need the
// protocol constants, not the full display wiring config.
#ifndef CONFIG_H
#define CONFIG_H
#define ESP_PACKET_START 0xAA
#define ESP_PACKET_END 0xAB
#define PACKET_ID_DISPLAY_DATA 0x31
#define PACKET_ID_ALERT_DATA 0x43
#define PACKET_ID_WRITE_USER_BYTES 0x13
#define PACKET_ID_TURN_OFF_DISPLAY 0x32
#define PACKET_ID_TURN_ON_DISPLAY 0x33
#define PACKET_ID_MUTE_ON 0x34
#define PACKET_ID_MUTE_OFF 0x35
#define PACKET_ID_REQ_WRITE_VOLUME 0x39
#define PACKET_ID_RESP_USER_BYTES 0x12
#define PACKET_ID_VERSION 0x01
#define PACKET_ID_RESP_VERSION 0x02
#define PACKET_ID_REQ_ALL_VOLUME 0x3C
#define PACKET_ID_RESP_ALL_VOLUME 0x3D
#endif

#include "../../src/packet_parser.h"
#include "../../src/packet_parser.cpp"
#include "../../src/packet_parser_alerts.cpp"

#ifndef ARDUINO
namespace {
uint32_t g_lastRecordedV1FwVersion = 0;
}
void perfRecordV1FirmwareVersion(uint32_t version) { g_lastRecordedV1FwVersion = version; }
#endif

#include <vector>

namespace {

std::vector<uint8_t> makePacket(uint8_t packetId, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> packet;
    packet.reserve(6 + payload.size());
    packet.push_back(ESP_PACKET_START);
    packet.push_back(0xDA);
    packet.push_back(0xE4);
    packet.push_back(packetId);
    packet.push_back(static_cast<uint8_t>(payload.size()));
    packet.insert(packet.end(), payload.begin(), payload.end());
    packet.push_back(ESP_PACKET_END);
    return packet;
}

std::vector<uint8_t> makeDisplayPayload(uint8_t bogeyByte,
                                        uint8_t barBitmap,
                                        uint8_t image1,
                                        uint8_t image2,
                                        uint8_t aux0 = 0,
                                        uint8_t aux1 = 0,
                                        uint8_t aux2 = 0,
                                        uint8_t bogeyByte2 = 0x00) {
    // V1 ESP protocol: last byte of payload region is the checksum.
    // Append a dummy checksum (0x00) to match the real wire format.
    return std::vector<uint8_t>{bogeyByte, bogeyByte2, barBitmap, image1, image2, aux0, aux1, aux2, 0x00};
}

std::vector<uint8_t> makeVersionPayload(char major,
                                        char minor,
                                        char rev1,
                                        char rev2,
                                        char ctrl) {
    // Spec-compliant V1 ESP version response payload (per
    // AndroidESPLibrary2 ResponseVersion.java): 7 ASCII bytes:
    //   [0] device letter, [1] major, [2] '.', [3] minor,
    //   [4] rev1, [5] rev2, [6] ctrl.
    return std::vector<uint8_t>{static_cast<uint8_t>('v'),
                                static_cast<uint8_t>(major),
                                static_cast<uint8_t>('.'),
                                static_cast<uint8_t>(minor),
                                static_cast<uint8_t>(rev1),
                                static_cast<uint8_t>(rev2),
                                static_cast<uint8_t>(ctrl)};
}

constexpr uint32_t kDefaultParseNowMs = 1000;

bool parsePacket(PacketParser& parser,
                 const std::vector<uint8_t>& packet,
                 uint32_t nowMs = kDefaultParseNowMs) {
    return parser.parse(packet.data(), packet.size(), nowMs);
}

template <size_t N>
bool parsePacket(PacketParser& parser,
                 const uint8_t (&packet)[N],
                 uint32_t nowMs = kDefaultParseNowMs) {
    return parser.parse(packet, N, nowMs);
}

}  // namespace

void setUp() {
#ifndef ARDUINO
    mockMillis = 0;
    mockMicros = 0;
#endif
}

void tearDown() {}

void test_parse_display_packet_updates_render_state() {
    PacketParser parser;
    const auto payload = makeDisplayPayload(
        static_cast<uint8_t>(115 | 0x80),  // 'P' with decimal point
        0x03,                              // 2 bars
        0x52,                              // Ka + side + mute
        0x42,                              // steady: Ka + side (mute not steady — flashing)
        0x04,                              // aux0: systemStatus=1 (V1 actively searching)
        0x04,                              // mode=A
        0x73);                             // main=7 mute=3
    const auto packet = makePacket(PACKET_ID_DISPLAY_DATA, payload);

    // Mute requires 2 consecutive display packets with bit set.
    // First packet: confirm count reaches 1 — not yet muted.
    TEST_ASSERT_TRUE(parsePacket(parser, packet));
    TEST_ASSERT_FALSE(parser.getDisplayState().muted);

    // Second packet: confirm count reaches 2 — now muted.
    TEST_ASSERT_TRUE(parsePacket(parser, packet));

    const DisplayState& state = parser.getDisplayState();
    TEST_ASSERT_EQUAL_UINT8(BAND_KA, state.activeBands);
    TEST_ASSERT_EQUAL_UINT8(DIR_SIDE, state.arrows);
    TEST_ASSERT_TRUE(state.muted);
    TEST_ASSERT_EQUAL_UINT8(3, state.signalBars);
    TEST_ASSERT_EQUAL('P', state.bogeyCounterChar);
    TEST_ASSERT_TRUE(state.bogeyCounterDot);
    // Per VR InfDisplayData.getMode(): when the V1 is alerting (bogey shows a
    // digit / 'P' / 'J' etc.) the mode cannot be determined. The auxData1
    // byte is NOT a mode source.
    TEST_ASSERT_FALSE(state.hasMode);
    TEST_ASSERT_EQUAL(0, state.modeChar);
    TEST_ASSERT_EQUAL_UINT8(7, state.mainVolume);
    TEST_ASSERT_EQUAL_UINT8(3, state.muteVolume);
    TEST_ASSERT_TRUE(state.hasVolumeData);
    TEST_ASSERT_EQUAL_UINT8(0x00, state.bandFlashBits);
    TEST_ASSERT_EQUAL_UINT8(0x00, state.flashBits);
}

void test_parse_display_packet_laser_keeps_led_bitmap_signal_bars() {
    PacketParser parser;
    const auto packet = makePacket(
        PACKET_ID_DISPLAY_DATA,
        makeDisplayPayload(63,
                           0x03,  // V1 source bitmap: 2 visible bars -> 3 local bars
                           0x21,  // Laser + front arrow
                           0x21,
                           0x04));

    TEST_ASSERT_TRUE(parsePacket(parser, packet));

    const DisplayState& state = parser.getDisplayState();
    TEST_ASSERT_EQUAL_UINT8(BAND_LASER, state.activeBands);
    TEST_ASSERT_EQUAL_UINT8(DIR_FRONT, state.arrows);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(3, state.signalBars,
        "laser display packets must use the normalized LED bitmap, not force synthetic full bars");
}

void test_parse_display_packet_zero_volume_does_not_force_muted() {
    PacketParser parser;
    // image1=0x20 (front arrow, no mute bit), aux2=0x00 (mainVol=0, muteVol=0)
    const auto packet = makePacket(
        PACKET_ID_DISPLAY_DATA,
        makeDisplayPayload(63, 0x01, 0x20, 0x20, 0x00, 0x00, 0x00));

    TEST_ASSERT_TRUE(parsePacket(parser, packet));
    // Mute state comes exclusively from image1 bit 4 — zero volume does not
    // imply muted.  This prevents false muted flashes when the checksum byte
    // is misread as volume data on short-payload display packets.
    TEST_ASSERT_FALSE(parser.getDisplayState().muted);
    TEST_ASSERT_EQUAL_UINT8(0, parser.getDisplayState().mainVolume);
}

// V1 protocol summary: docs/V1_PROTOCOL_REFERENCES.md#infdisplaydata.
// FSD-002 verdict reversal: the V1 bogey counter is a SINGLE 7-segment LED. payload[0]
// (image1) is the steady displayed character;
// payload[1] (image2) is the blink-off mask companion to payload[0]. The
// parser captures both bytes so that future renderer enhancements can drive a
// blink animation off image1 & ~image2 (the same convention already used for
// band/arrow flash bits at packet_parser.cpp `flashingBits = image1 & ~image2`).
//
// These tests pin the protocol-correct semantics: image2 is captured for
// blink-mask use, NOT for second-digit rendering. The renderer in
// display_update.cpp / display_screens.cpp passes only image1 to
// drawTopCounterPair — see the FSD-002 Verdict Reversal "Corrective action".
void test_parse_display_packet_captures_bogey_image2_for_blink_mask() {
    PacketParser parser;
    // Steady "1" on the LED: image1 = '1' encoded as 0x06 (segments b+c),
    // image2 = '1' encoded the same way, no decimal point on either byte.
    // Expected: bogeyCounterByte == bogeyCounterByte2, both decode to '1',
    // image1 & ~image2 == 0 (no blinking segments).
    const auto packet = makePacket(
        PACKET_ID_DISPLAY_DATA,
        makeDisplayPayload(6, 0x01, 0x20, 0x20, 0x00, 0x00, 0x00, 6));

    TEST_ASSERT_TRUE(parsePacket(parser, packet));

    const DisplayState& state = parser.getDisplayState();
    TEST_ASSERT_EQUAL_UINT8(6, state.bogeyCounterByte);
    TEST_ASSERT_EQUAL('1', state.bogeyCounterChar);
    TEST_ASSERT_FALSE(state.bogeyCounterDot);
    TEST_ASSERT_EQUAL_UINT8(6, state.bogeyCounterByte2);
    TEST_ASSERT_EQUAL('1', state.bogeyCounterChar2);
    TEST_ASSERT_FALSE(state.bogeyCounterDot2);
    // Blink-mask invariant: identical image1/image2 → no segments blinking.
    TEST_ASSERT_EQUAL_UINT8(0, static_cast<uint8_t>(state.bogeyCounterByte & ~state.bogeyCounterByte2));
}

void test_parse_display_packet_captures_blinking_bogey_indicator() {
    PacketParser parser;
    // Blinking "J" on the LED: V1's natural junk-out indicator behavior.
    // image1 = 'J' encoded byte (lit), image2 = 0 (dark). The parser must
    // capture both bytes so a future renderer can drive blink animation. The
    // currently-shipping renderer reads only image1 and shows steady J — that
    // is acceptable (matches V1's on-phase) but does not yet drive blink.
    const uint8_t junkByte = 0x1E;  // segments b+c+d+e — J on a 7-seg LED
    const auto packet = makePacket(
        PACKET_ID_DISPLAY_DATA,
        makeDisplayPayload(junkByte, 0x01, 0x20, 0x20, 0x00, 0x00, 0x00, 0x00));

    TEST_ASSERT_TRUE(parsePacket(parser, packet));

    const DisplayState& state = parser.getDisplayState();
    TEST_ASSERT_EQUAL_UINT8(junkByte, state.bogeyCounterByte);
    TEST_ASSERT_EQUAL_UINT8(0x00, state.bogeyCounterByte2);
    // Blink-mask invariant: image1 lit, image2 dark → all of image1's segments
    // are blinking. This is the "J flashing for junk-out" case.
    TEST_ASSERT_EQUAL_UINT8(junkByte,
        static_cast<uint8_t>(state.bogeyCounterByte & ~state.bogeyCounterByte2));
}

void test_parse_packet_rejects_six_byte_frame() {
    PacketParser parser;
    const uint8_t packet[] = {0xAA, 0xDA, 0xE4, 0x31, 0x00, 0xAB};
    TEST_ASSERT_FALSE(parser.validatePacketForTest(packet, sizeof(packet)));
    TEST_ASSERT_FALSE(parsePacket(parser, packet));
}

void test_parse_packet_rejects_seven_byte_frame() {
    PacketParser parser;
    const uint8_t packet[] = {0xAA, 0xDA, 0xE4, 0x31, 0x01, 0x00, 0xAB};
    TEST_ASSERT_FALSE(parser.validatePacketForTest(packet, sizeof(packet)));
    TEST_ASSERT_FALSE(parsePacket(parser, packet));
}

void test_parse_packet_rejects_bad_framing() {
    PacketParser parser;
    const auto packet = makePacket(PACKET_ID_DISPLAY_DATA, makeDisplayPayload(63, 0x01, 0x20, 0x20));

    std::vector<uint8_t> badStart = packet;
    badStart.front() = 0xBB;
    TEST_ASSERT_FALSE(parsePacket(parser, badStart));

    std::vector<uint8_t> badEnd = packet;
    badEnd.back() = 0xAC;
    TEST_ASSERT_FALSE(parsePacket(parser, badEnd));
}

void test_parse_version_packet_records_supported_volume_version() {
    PacketParser parser;
    const auto packet = makePacket(PACKET_ID_RESP_VERSION, makeVersionPayload('4', '1', '0', '2', '8'));

    TEST_ASSERT_TRUE(parsePacket(parser, packet));

    const DisplayState& state = parser.getDisplayState();
    TEST_ASSERT_TRUE(state.hasV1Version);
    TEST_ASSERT_EQUAL_UINT32(41028, state.v1FirmwareVersion);
    TEST_ASSERT_TRUE(state.supportsVolume());
    // Parser must publish the version into perf-metrics so SD logs can
    // surface the connected V1's firmware identity.
    TEST_ASSERT_EQUAL_UINT32(41028, g_lastRecordedV1FwVersion);
}

void test_parse_version_packet_ignores_non_digit_payload() {
    PacketParser parser;
    // Spec-compliant letter and dot, but rev1 byte is a non-digit (0xFF).
    const auto packet = makePacket(
        PACKET_ID_RESP_VERSION,
        {static_cast<uint8_t>('v'), static_cast<uint8_t>('4'), static_cast<uint8_t>('.'),
         static_cast<uint8_t>('1'), 0xFF, static_cast<uint8_t>('2'), static_cast<uint8_t>('8')});

    TEST_ASSERT_TRUE(parsePacket(parser, packet));

    const DisplayState& state = parser.getDisplayState();
    TEST_ASSERT_FALSE(state.hasV1Version);
    TEST_ASSERT_EQUAL_UINT32(0, state.v1FirmwareVersion);
    TEST_ASSERT_FALSE(state.supportsVolume());
}

void test_parse_version_packet_ignores_short_payload() {
    PacketParser parser;
    // 6-byte payload (one byte short of the spec-required 7).
    const uint8_t packet[] = {
        ESP_PACKET_START,
        0xDA,
        0xE4,
        PACKET_ID_RESP_VERSION,
        0x06,
        static_cast<uint8_t>('v'),
        static_cast<uint8_t>('4'),
        static_cast<uint8_t>('.'),
        static_cast<uint8_t>('1'),
        static_cast<uint8_t>('0'),
        static_cast<uint8_t>('2'),
        ESP_PACKET_END
    };

    TEST_ASSERT_TRUE(parsePacket(parser, packet));

    const DisplayState& state = parser.getDisplayState();
    TEST_ASSERT_FALSE(state.hasV1Version);
    TEST_ASSERT_EQUAL_UINT32(0, state.v1FirmwareVersion);
}

void test_parse_version_packet_preserves_prior_valid_version_on_malformed_followup() {
    PacketParser parser;
    const auto validPacket = makePacket(PACKET_ID_RESP_VERSION, makeVersionPayload('4', '1', '0', '3', '5'));
    const auto malformedPacket = makePacket(
        PACKET_ID_RESP_VERSION,
        {static_cast<uint8_t>('v'), static_cast<uint8_t>('4'), static_cast<uint8_t>('.'),
         static_cast<uint8_t>('1'), 0xFF, static_cast<uint8_t>('9'), static_cast<uint8_t>('9')});

    TEST_ASSERT_TRUE(parsePacket(parser, validPacket));
    TEST_ASSERT_TRUE(parsePacket(parser, malformedPacket));

    const DisplayState& state = parser.getDisplayState();
    TEST_ASSERT_TRUE(state.hasV1Version);
    TEST_ASSERT_EQUAL_UINT32(41035, state.v1FirmwareVersion);
}

// Regression for V1-parsing review item #1 / #2 (April 2026): ensure the
// outbound REQVERSION id (0x01) is never decoded as a version reply, and that
// non-V1 device letters (e.g. 'C' for Concealed Display) are ignored.
void test_parse_version_packet_rejects_request_id() {
    PacketParser parser;
    const auto packet = makePacket(PACKET_ID_VERSION, makeVersionPayload('4', '1', '0', '2', '8'));
    TEST_ASSERT_TRUE(parsePacket(parser, packet));
    const DisplayState& state = parser.getDisplayState();
    TEST_ASSERT_FALSE(state.hasV1Version);
    TEST_ASSERT_EQUAL_UINT32(0, state.v1FirmwareVersion);
}

void test_parse_version_packet_ignores_non_v1_device_letter() {
    PacketParser parser;
    // 'C' = Concealed Display reply; we only record the main V1 firmware.
    const auto packet = makePacket(
        PACKET_ID_RESP_VERSION,
        {static_cast<uint8_t>('C'), static_cast<uint8_t>('4'), static_cast<uint8_t>('.'),
         static_cast<uint8_t>('1'), static_cast<uint8_t>('0'), static_cast<uint8_t>('2'),
         static_cast<uint8_t>('8')});
    TEST_ASSERT_TRUE(parsePacket(parser, packet));
    const DisplayState& state = parser.getDisplayState();
    TEST_ASSERT_FALSE(state.hasV1Version);
}

void test_parse_display_ack_packets_toggle_display_state() {
    PacketParser parser;
    const auto darkPacket = makePacket(PACKET_ID_TURN_OFF_DISPLAY, {0x00, 0x00});
    const auto lightPacket = makePacket(PACKET_ID_TURN_ON_DISPLAY, {0x00});

    TEST_ASSERT_TRUE(parsePacket(parser, darkPacket));
    TEST_ASSERT_FALSE(parser.getDisplayState().displayOn);
    TEST_ASSERT_TRUE(parser.getDisplayState().hasDisplayOn);

    TEST_ASSERT_TRUE(parsePacket(parser, lightPacket));
    TEST_ASSERT_TRUE(parser.getDisplayState().displayOn);
}

// Mode is decoded from the bogey-counter 7-segment
// glyph (payload[0] & 0x7F), not from any aux byte. These tests pin every
// mode character documented in Valentine's InfDisplayData.getMode().
void test_parse_display_packet_decodes_mode_from_bogey_glyph() {
    struct Case {
        uint8_t bogeyByte;  // raw 7-seg pattern (no DP)
        char expected;
    };
    const Case cases[] = {
        {0x77, 'A'}, {0x39, 'C'}, {0x3E, 'U'},
        {0x18, 'l'}, {0x1C, 'u'}, {0x58, 'c'},
        {0x38, 'L'},
    };
    for (const auto& c : cases) {
        PacketParser parser;
        const auto payload = makeDisplayPayload(c.bogeyByte, 0x00, 0x00, 0x00,
                                                0x00, 0xFF, 0x00);
        // Aux1 is intentionally 0xFF to prove the old (aux1>>2)&0x03 source
        // is no longer consulted.
        const auto packet = makePacket(PACKET_ID_DISPLAY_DATA, payload);
        TEST_ASSERT_TRUE(parsePacket(parser, packet));
        const DisplayState& state = parser.getDisplayState();
        TEST_ASSERT_TRUE(state.hasMode);
        TEST_ASSERT_EQUAL(c.expected, state.modeChar);
    }
}

void test_parse_display_packet_mode_unknown_when_bogey_is_digit() {
    PacketParser parser;
    // Bogey '0' (raw 0x3F) -> alerting; mode is not determinable.
    const auto payload = makeDisplayPayload(0x3F, 0x00, 0x00, 0x00,
                                            0x00, 0xFF, 0x00);
    const auto packet = makePacket(PACKET_ID_DISPLAY_DATA, payload);
    TEST_ASSERT_TRUE(parsePacket(parser, packet));
    const DisplayState& state = parser.getDisplayState();
    TEST_ASSERT_FALSE(state.hasMode);
    TEST_ASSERT_EQUAL(0, state.modeChar);
}

// Spec-correct audio mute is auxData0 bit 0
// (Valentine InfDisplayData.isSoft). Independent of the LED-derived `muted`.
void test_parse_display_packet_softmuted_tracks_aux0_bit_0() {
    PacketParser parser;
    // aux0 = 0x01 -> isSoft() == true; image1 has no mute LED bit (0x10).
    const auto on = makePacket(PACKET_ID_DISPLAY_DATA,
                               makeDisplayPayload(0x77, 0x00, 0x20, 0x20, 0x01));
    TEST_ASSERT_TRUE(parsePacket(parser, on));
    TEST_ASSERT_TRUE(parser.getDisplayState().softMuted);
    TEST_ASSERT_FALSE(parser.getDisplayState().muted);  // LED mute not set

    const auto off = makePacket(PACKET_ID_DISPLAY_DATA,
                                makeDisplayPayload(0x77, 0x00, 0x20, 0x20, 0x00));
    TEST_ASSERT_TRUE(parsePacket(parser, off));
    TEST_ASSERT_FALSE(parser.getDisplayState().softMuted);
}

void test_parse_display_packet_softmuted_independent_of_led_mute() {
    PacketParser parser;
    // image1 bit 4 (0x10) set (LED mute icon) but aux0 bit 0 clear.
    const auto p = makeDisplayPayload(0x77, 0x00, 0x30, 0x20, 0x00);
    const auto pkt = makePacket(PACKET_ID_DISPLAY_DATA, p);
    TEST_ASSERT_TRUE(parsePacket(parser, pkt));
    TEST_ASSERT_TRUE(parsePacket(parser, pkt));  // debounce
    TEST_ASSERT_TRUE(parser.getDisplayState().muted);        // LED-debounced
    TEST_ASSERT_FALSE(parser.getDisplayState().softMuted);   // spec audio mute
}

// Bands and arrows must be suppressed when the V1
// reports system-status=false (aux0 bit 2 clear).
void test_parse_display_packet_suppresses_bands_when_system_status_clear() {
    PacketParser parser;
    // image1=0x22 → Ka band + side arrow. aux0=0x00 → systemStatus=false.
    const auto pkt = makePacket(PACKET_ID_DISPLAY_DATA,
                                makeDisplayPayload(0x77, 0x03, 0x22, 0x22, 0x00));
    TEST_ASSERT_TRUE(parsePacket(parser, pkt));
    const DisplayState& s = parser.getDisplayState();
    TEST_ASSERT_FALSE(s.systemStatus);
    TEST_ASSERT_EQUAL_UINT8(BAND_NONE, s.activeBands);
    TEST_ASSERT_EQUAL_UINT8(DIR_NONE, s.arrows);
    TEST_ASSERT_EQUAL_UINT8(0, s.bandFlashBits);
    TEST_ASSERT_EQUAL_UINT8(0, s.flashBits);
}

void test_parse_display_packet_reports_bands_when_system_status_set() {
    PacketParser parser;
    // image1=0x42 -> Ka band + side arrow. aux0=0x04 -> systemStatus=true.
    const auto pkt = makePacket(PACKET_ID_DISPLAY_DATA,
                                makeDisplayPayload(0x77, 0x03, 0x42, 0x42, 0x04));
    TEST_ASSERT_TRUE(parsePacket(parser, pkt));
    const DisplayState& s = parser.getDisplayState();
    TEST_ASSERT_TRUE(s.systemStatus);
    TEST_ASSERT_EQUAL_UINT8(BAND_KA, s.activeBands);
    TEST_ASSERT_EQUAL_UINT8(DIR_SIDE, s.arrows);
}

// A display packet whose payload is shorter than 8 bytes (the minimum that
// includes aux0/aux1/aux2) MUST be rejected before we can read aux0. This
// pins the invariant that allows parseDisplayData's `length < 8` guard to
// own the aux-byte safety: no caller in production reaches the
// length<=5 branch with `auxSystemStatus` undecided. Regressing this
// guard would re-expose the systemStatus default flagged in CODE_REVIEW.md
// item M1 / #8, where a truncated packet would have admitted unread aux0
// state into the display pipeline.
void test_parse_display_packet_rejects_short_payload() {
    PacketParser parser;
    // First land a known-good display state so we can detect any leakage
    // from a subsequent short-payload parse. After this, systemStatus=true
    // and activeBands=BAND_KA (image1=0x42 reports Ka + side w/ aux0=0x04).
    const auto goodPkt = makePacket(PACKET_ID_DISPLAY_DATA,
                                    makeDisplayPayload(0x77, 0x03, 0x42, 0x42, 0x04));
    TEST_ASSERT_TRUE(parsePacket(parser, goodPkt));
    TEST_ASSERT_TRUE(parser.getDisplayState().systemStatus);
    TEST_ASSERT_EQUAL_UINT8(BAND_KA, parser.getDisplayState().activeBands);

    // Now try a 7-byte payload (one short of aux0). makePacket frames it:
    // start + dst + src + id + len(=7) + 7 bytes + end = 13-byte packet,
    // so validatePacket (>=8) accepts it; parseDisplayData should reject
    // because the post-strip length (7) is less than 8.
    const std::vector<uint8_t> shortPayload = {
        0x77,  // bogey
        0x00,  // bogey2
        0x03,  // bar bitmap
        0x22,  // image1 (Ka + side) — must NOT be read
        0x22,  // image2
        0x00,  // would-be aux0 — must NOT be read
        0x00   // trailing byte
    };
    const auto shortPkt = makePacket(PACKET_ID_DISPLAY_DATA, shortPayload);
    TEST_ASSERT_FALSE(parsePacket(parser, shortPkt));

    // Display state must be unchanged by the rejected parse.
    const DisplayState& s = parser.getDisplayState();
    TEST_ASSERT_TRUE(s.systemStatus);
    TEST_ASSERT_EQUAL_UINT8(BAND_KA, s.activeBands);
}

// Boundary case at the minimum accepted payload length (8 bytes): with
// aux0=0, systemStatus must be cleared. This pins the post-fix default
// from CODE_REVIEW.md item #8 — the prior `auxSystemStatus = true` default
// would silently flip this assertion if reintroduced.
void test_parse_display_packet_min_payload_clears_system_status_when_aux0_zero() {
    PacketParser parser;
    // makeDisplayPayload yields 9 bytes; parseDisplayData sees 9 here, but the
    // post-strip length is 9 which is >5, exercising the same code path as
    // the rest of the production stream while explicitly asserting the
    // bit-clear default.
    const auto pkt = makePacket(PACKET_ID_DISPLAY_DATA,
                                makeDisplayPayload(0x77, 0x00, 0x00, 0x00, 0x00));
    TEST_ASSERT_TRUE(parsePacket(parser, pkt));
    const DisplayState& s = parser.getDisplayState();
    TEST_ASSERT_FALSE(s.systemStatus);
    TEST_ASSERT_FALSE(s.softMuted);
}

// aux0 bit 3 → displayOn is intentionally not consumed
// because it can hide alerts during V1 dark mode. The
// 0x32 / 0x33 ACK path still drives displayOn — see
// test_parse_display_ack_packets_toggle_display_state above.

// Spec-compliant RESPALLVOLUME 0x3D parser.
// The 4-byte payload [main, muted, savedMain, savedMuted] populates
// mainVolume/muteVolume (overriding aux2 inference) plus the new
// savedMainVolume/savedMuteVolume pair, and sets hasSavedVolume=true.
void test_parse_resp_all_volume_populates_volume_fields() {
    PacketParser parser;
    const std::vector<uint8_t> payload = {0x07, 0x02, 0x09, 0x03};  // main=7 muted=2 savedMain=9 savedMuted=3
    const auto pkt = makePacket(PACKET_ID_RESP_ALL_VOLUME, payload);
    TEST_ASSERT_TRUE(parsePacket(parser, pkt));
    const DisplayState& s = parser.getDisplayState();
    TEST_ASSERT_EQUAL_UINT8(7, s.mainVolume);
    TEST_ASSERT_EQUAL_UINT8(2, s.muteVolume);
    TEST_ASSERT_EQUAL_UINT8(9, s.savedMainVolume);
    TEST_ASSERT_EQUAL_UINT8(3, s.savedMuteVolume);
    TEST_ASSERT_TRUE(s.hasVolumeData);
    TEST_ASSERT_TRUE(s.hasSavedVolume);
}

// RESPALLVOLUME must overwrite the aux2-nibble inference set by an earlier
// display packet — it is the authoritative source per VR spec.
void test_resp_all_volume_overrides_display_aux2_inference() {
    PacketParser parser;
    // First, send a display packet with aux2=0x73 (main=7 mute=3) — fallback path.
    const auto disp = makePacket(PACKET_ID_DISPLAY_DATA,
                                 makeDisplayPayload(0x77, 0x00, 0x20, 0x20, 0x04, 0x00, 0x73));
    TEST_ASSERT_TRUE(parsePacket(parser, disp));
    TEST_ASSERT_EQUAL_UINT8(7, parser.getDisplayState().mainVolume);
    TEST_ASSERT_EQUAL_UINT8(3, parser.getDisplayState().muteVolume);
    TEST_ASSERT_FALSE(parser.getDisplayState().hasSavedVolume);

    // Now RESPALLVOLUME with different values — should win.
    const auto vol = makePacket(PACKET_ID_RESP_ALL_VOLUME,
                                std::vector<uint8_t>{0x05, 0x01, 0x08, 0x04});
    TEST_ASSERT_TRUE(parsePacket(parser, vol));
    TEST_ASSERT_EQUAL_UINT8(5, parser.getDisplayState().mainVolume);
    TEST_ASSERT_EQUAL_UINT8(1, parser.getDisplayState().muteVolume);
    TEST_ASSERT_EQUAL_UINT8(8, parser.getDisplayState().savedMainVolume);
    TEST_ASSERT_EQUAL_UINT8(4, parser.getDisplayState().savedMuteVolume);
    TEST_ASSERT_TRUE(parser.getDisplayState().hasSavedVolume);
}

void test_parse_resp_all_volume_rejects_short_payload() {
    PacketParser parser;
    // Only 3 bytes of payload — must not populate saved-volume fields.
    const auto pkt = makePacket(PACKET_ID_RESP_ALL_VOLUME,
                                std::vector<uint8_t>{0x07, 0x02, 0x09});
    TEST_ASSERT_TRUE(parsePacket(parser, pkt));
    const DisplayState& s = parser.getDisplayState();
    TEST_ASSERT_FALSE(s.hasSavedVolume);
    TEST_ASSERT_EQUAL_UINT8(0, s.savedMainVolume);
    TEST_ASSERT_EQUAL_UINT8(0, s.savedMuteVolume);
}

// VR InfDisplayData.getNumberOfLEDS() returns a full-bar overflow
// sentinel (meaning "show all bars") for any non-standard signal-bar bitmap.
// We previously used __builtin_popcount, which would report fractional counts
// for stray patterns (e.g. 0x80 -> 1 bar instead of "all"). Pin the local
// 8-slot display behavior so V1 source full-scale (0x3F, six visible bars)
// renders as all eight local bars instead of understating the alert.
void test_decode_signal_bars_expands_v1_source_meter_to_local_eight_slot_meter() {
    PacketParser parser;
    // image1 = front arrow only, aux0 systemStatus=1 so bands/bars are reported.
    // Standard V1 Gen2 ramp values (0x01..0x3F) and 0 are the expected source
    // bitmaps. 0x7F/0xFF and anything non-contiguous mean overflow / show all
    // eight local bars.
    struct Case { uint8_t bitmap; uint8_t expected; };
    const Case cases[] = {
        {0x80, 8},  // single high bit — popcount would return 1
        {0xAA, 8},  // alternating — popcount would return 4
        {0x55, 8},  // alternating — popcount would return 4
        {0x02, 8},  // single mid bit — popcount would return 1
        {0xF0, 8},  // upper nibble — popcount would return 4
        {0x00, 0},  // explicit zero stays zero
        {0x01, 1}, {0x03, 3}, {0x07, 4}, {0x0F, 5},
        {0x1F, 7}, {0x3F, 8}, {0x7F, 8}, {0xFF, 8},
    };
    for (const auto& c : cases) {
        const auto packet = makePacket(
            PACKET_ID_DISPLAY_DATA,
            makeDisplayPayload(63, c.bitmap, 0x20, 0x20, 0x04, 0x00, 0x00));
        TEST_ASSERT_TRUE(parsePacket(parser, packet));
        const DisplayState& s = parser.getDisplayState();
        char msg[64];
        std::snprintf(msg, sizeof(msg), "bitmap=0x%02X expected=%u got=%u",
                      c.bitmap, c.expected, s.signalBars);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(c.expected, s.signalBars, msg);
    }
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_parse_display_packet_updates_render_state);
    RUN_TEST(test_parse_display_packet_laser_keeps_led_bitmap_signal_bars);
    RUN_TEST(test_parse_display_packet_zero_volume_does_not_force_muted);
    RUN_TEST(test_parse_display_packet_captures_bogey_image2_for_blink_mask);
    RUN_TEST(test_parse_display_packet_captures_blinking_bogey_indicator);
    RUN_TEST(test_parse_packet_rejects_six_byte_frame);
    RUN_TEST(test_parse_packet_rejects_seven_byte_frame);
    RUN_TEST(test_parse_packet_rejects_bad_framing);
    RUN_TEST(test_parse_version_packet_records_supported_volume_version);
    RUN_TEST(test_parse_version_packet_ignores_non_digit_payload);
    RUN_TEST(test_parse_version_packet_ignores_short_payload);
    RUN_TEST(test_parse_version_packet_preserves_prior_valid_version_on_malformed_followup);
    RUN_TEST(test_parse_version_packet_rejects_request_id);
    RUN_TEST(test_parse_version_packet_ignores_non_v1_device_letter);
    RUN_TEST(test_parse_display_ack_packets_toggle_display_state);
    RUN_TEST(test_parse_display_packet_decodes_mode_from_bogey_glyph);
    RUN_TEST(test_parse_display_packet_mode_unknown_when_bogey_is_digit);
    RUN_TEST(test_parse_display_packet_softmuted_tracks_aux0_bit_0);
    RUN_TEST(test_parse_display_packet_softmuted_independent_of_led_mute);
    RUN_TEST(test_parse_display_packet_suppresses_bands_when_system_status_clear);
    RUN_TEST(test_parse_display_packet_rejects_short_payload);
    RUN_TEST(test_parse_display_packet_min_payload_clears_system_status_when_aux0_zero);
    RUN_TEST(test_parse_display_packet_reports_bands_when_system_status_set);
    RUN_TEST(test_parse_resp_all_volume_populates_volume_fields);
    RUN_TEST(test_resp_all_volume_overrides_display_aux2_inference);
    RUN_TEST(test_parse_resp_all_volume_rejects_short_payload);
    RUN_TEST(test_decode_signal_bars_expands_v1_source_meter_to_local_eight_slot_meter);
    return UNITY_END();
}
