#include <unity.h>

#include "../mocks/Arduino.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

// packet_parser.cpp includes ../include/config.h for packet IDs.
// In native tests we only need protocol constants, not display driver wiring.
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

// Stubs for symbols pulled in via packet_parser.cpp's perf hook.
// Native parser tests don't link perf_metrics.cpp, so provide a no-op.
#ifndef ARDUINO
namespace {
uint32_t g_lastRecordedV1FwVersion = 0;
}
void perfRecordV1FirmwareVersion(uint32_t version) { g_lastRecordedV1FwVersion = version; }
#endif

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace {

std::string readTextFile(const char* path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        return std::string();
    }
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::vector<uint8_t> makePacket(uint8_t packetId, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> packet;
    packet.reserve(6 + payload.size());
    packet.push_back(ESP_PACKET_START);
    packet.push_back(0xDA);  // Dest (not validated by parser)
    packet.push_back(0xE4);  // Orig (not validated by parser)
    packet.push_back(packetId);
    packet.push_back(static_cast<uint8_t>(payload.size()));  // Length hint (not enforced)
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
    // payload[0]=bogey1, [1]=bogey2, [2]=bars, [3]=image1, [4]=image2, [5..7]=aux
    return std::vector<uint8_t>{bogeyByte, bogeyByte2, barBitmap, image1, image2, aux0, aux1, aux2};
}

std::vector<uint8_t> makeVersionPayload(char major,
                                        char minor,
                                        char rev1,
                                        char rev2,
                                        char ctrl) {
    // Spec-compliant: [letter, major, '.', minor, rev1, rev2, ctrl].
    return std::vector<uint8_t>{static_cast<uint8_t>('v'),
                                static_cast<uint8_t>(major),
                                static_cast<uint8_t>('.'),
                                static_cast<uint8_t>(minor),
                                static_cast<uint8_t>(rev1),
                                static_cast<uint8_t>(rev2),
                                static_cast<uint8_t>(ctrl)};
}

std::vector<uint8_t> makeAlertPayload(uint8_t index,
                                      uint8_t count,
                                      uint16_t freqMHz,
                                      uint8_t frontRaw,
                                      uint8_t rearRaw,
                                      uint8_t bandArrow,
                                      uint8_t aux0) {
    const uint8_t indexCount = static_cast<uint8_t>(((index & 0x0F) << 4) | (count & 0x0F));
    return std::vector<uint8_t>{
        indexCount,
        static_cast<uint8_t>((freqMHz >> 8) & 0xFF),
        static_cast<uint8_t>(freqMHz & 0xFF),
        frontRaw,
        rearRaw,
        bandArrow,
        aux0
    };
}

void assertContainsFrequencies(const PacketParser& parser, uint16_t a, uint16_t b) {
    const auto& alerts = parser.getAllAlerts();
    const size_t count = parser.getAlertCount();
    TEST_ASSERT_EQUAL_UINT32(2, static_cast<uint32_t>(count));

    bool foundA = false;
    bool foundB = false;
    for (size_t i = 0; i < count; ++i) {
        if (alerts[i].frequency == a) foundA = true;
        if (alerts[i].frequency == b) foundB = true;
    }
    TEST_ASSERT_TRUE(foundA);
    TEST_ASSERT_TRUE(foundB);
}

void assertContainsThreeFrequencies(const PacketParser& parser, uint16_t a, uint16_t b, uint16_t c) {
    const auto& alerts = parser.getAllAlerts();
    const size_t count = parser.getAlertCount();
    TEST_ASSERT_EQUAL_UINT32(3, static_cast<uint32_t>(count));

    bool foundA = false;
    bool foundB = false;
    bool foundC = false;
    for (size_t i = 0; i < count; ++i) {
        if (alerts[i].frequency == a) foundA = true;
        if (alerts[i].frequency == b) foundB = true;
        if (alerts[i].frequency == c) foundC = true;
    }
    TEST_ASSERT_TRUE(foundA);
    TEST_ASSERT_TRUE(foundB);
    TEST_ASSERT_TRUE(foundC);
}

size_t countFrequency(const PacketParser& parser, uint16_t frequency) {
    const auto& alerts = parser.getAllAlerts();
    const size_t count = parser.getAlertCount();
    size_t matches = 0;
    for (size_t i = 0; i < count; ++i) {
        if (alerts[i].isValid && alerts[i].frequency == frequency) {
            ++matches;
        }
    }
    return matches;
}

constexpr uint32_t kDefaultParseNowMs = 1000;

bool parsePacket(PacketParser& parser,
                 const std::vector<uint8_t>& packet,
                 uint32_t nowMs = kDefaultParseNowMs) {
    return parser.parse(packet.data(), packet.size(), nowMs);
}

}  // namespace

void setUp() {
#ifndef ARDUINO
    mockMillis = 0;
    mockMicros = 0;
#endif
}
void tearDown() {}

void test_display_stream_decodes_junk_counter_char() {
    PacketParser parser;

    // 30 = 'J', with decimal point bit set.
    const auto payload = makeDisplayPayload(static_cast<uint8_t>(30 | 0x80), 0x03, 0x24, 0x24);
    const auto packet = makePacket(PACKET_ID_DISPLAY_DATA, payload);

    TEST_ASSERT_TRUE(parsePacket(parser, packet));
    const DisplayState& state = parser.getDisplayState();
    TEST_ASSERT_EQUAL('J', state.bogeyCounterChar);
    TEST_ASSERT_TRUE(state.bogeyCounterDot);
}

// Per ESP Spec 3.003 page 25 + FSD-002 Verdict Reversal: payload[1] (image2)
// is the blink-off mask companion to payload[0] (image1) of a SINGLE 7-segment
// LED, not a second physical digit. The parser must capture both bytes; the
// renderer uses image1 as the steady displayed character.
//
// This test exercises the wire-decoding round-trip: any byte values may appear
// on either image plane (they're both raw 7-segment encodings), and the
// decoder must produce the same character regardless of which plane the byte
// arrived on. It does NOT assert that the renderer composes the two values
// into a side-by-side display — that interpretation was the FSD-002 misread.
void test_display_stream_decodes_both_bogey_image_planes() {
    PacketParser parser;

    const auto payload = makeDisplayPayload(6, 0x03, 0x24, 0x24, 0x00, 0x00, 0x00, static_cast<uint8_t>(79 | 0x80));
    const auto packet = makePacket(PACKET_ID_DISPLAY_DATA, payload);

    TEST_ASSERT_TRUE(parsePacket(parser, packet));
    const DisplayState& state = parser.getDisplayState();
    // image1 round-trip
    TEST_ASSERT_EQUAL('1', state.bogeyCounterChar);
    TEST_ASSERT_FALSE(state.bogeyCounterDot);
    // image2 round-trip — captured for blink-mask use
    TEST_ASSERT_EQUAL('3', state.bogeyCounterChar2);
    TEST_ASSERT_TRUE(state.bogeyCounterDot2);
}

void test_alert_stream_out_of_order_rows_completes_table() {
    PacketParser parser;

    // Count=2, send index 2 first then index 1.
    const auto row2 = makePacket(PACKET_ID_ALERT_DATA, makeAlertPayload(2, 2, 24150, 0x90, 0x80, 0x84, 0x00));
    const auto row1 = makePacket(PACKET_ID_ALERT_DATA, makeAlertPayload(1, 2, 34700, 0xB0, 0x00, 0x22, 0x80));

    TEST_ASSERT_TRUE(parsePacket(parser, row2));
    TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(parser.getAlertCount()));

    TEST_ASSERT_TRUE(parsePacket(parser, row1));
    TEST_ASSERT_EQUAL_UINT32(2, static_cast<uint32_t>(parser.getAlertCount()));
    TEST_ASSERT_TRUE(parser.hasAlerts());
    assertContainsFrequencies(parser, 24150, 34700);
}

void test_alert_stream_duplicate_index_replaces_prior_row() {
    PacketParser parser;

    const auto row1a = makePacket(PACKET_ID_ALERT_DATA, makeAlertPayload(1, 2, 24150, 0x90, 0x80, 0x84, 0x00));
    const auto row1b = makePacket(PACKET_ID_ALERT_DATA, makeAlertPayload(1, 2, 24152, 0x92, 0x81, 0x84, 0x00));
    const auto row2 = makePacket(PACKET_ID_ALERT_DATA, makeAlertPayload(2, 2, 34700, 0xB0, 0x00, 0x22, 0x80));

    TEST_ASSERT_TRUE(parsePacket(parser, row1a));
    TEST_ASSERT_TRUE(parsePacket(parser, row1b));
    TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(parser.getAlertCount()));
    TEST_ASSERT_TRUE(parsePacket(parser, row2));

    TEST_ASSERT_TRUE(parser.hasAlerts());
    TEST_ASSERT_EQUAL_UINT32(2, static_cast<uint32_t>(parser.getAlertCount()));
    assertContainsFrequencies(parser, 24152, 34700);
}

void test_alert_stream_zero_based_index_is_supported() {
    PacketParser parser;

    const auto row0 = makePacket(PACKET_ID_ALERT_DATA, makeAlertPayload(0, 2, 24150, 0x90, 0x80, 0x84, 0x00));
    const auto row1 = makePacket(PACKET_ID_ALERT_DATA, makeAlertPayload(1, 2, 34700, 0xB0, 0x00, 0x22, 0x80));
    const auto row2 = makePacket(PACKET_ID_ALERT_DATA, makeAlertPayload(2, 2, 33800, 0xA0, 0x00, 0x22, 0x00));

    TEST_ASSERT_TRUE(parsePacket(parser, row0));
    TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(parser.getAlertCount()));

    TEST_ASSERT_TRUE(parsePacket(parser, row1));
    TEST_ASSERT_EQUAL_UINT32(2, static_cast<uint32_t>(parser.getAlertCount()));
    assertContainsFrequencies(parser, 24150, 34700);

    TEST_ASSERT_TRUE(parsePacket(parser, row2));
    TEST_ASSERT_EQUAL_UINT32(2, static_cast<uint32_t>(parser.getAlertCount()));
    assertContainsFrequencies(parser, 24150, 34700);
}

void test_alert_stream_ambiguous_first_row_does_not_lock_wrong_mode() {
    PacketParser parser;

    // idx=1/count=2 is ambiguous (valid in both schemes). Follow with idx=0 to
    // complete zero-based table and verify we do not wait for one-based idx=2.
    const auto row1 = makePacket(PACKET_ID_ALERT_DATA, makeAlertPayload(1, 2, 34700, 0xB0, 0x00, 0x22, 0x80));
    const auto row0 = makePacket(PACKET_ID_ALERT_DATA, makeAlertPayload(0, 2, 24150, 0x90, 0x80, 0x84, 0x00));
    const auto row2 = makePacket(PACKET_ID_ALERT_DATA, makeAlertPayload(2, 2, 33800, 0xA0, 0x00, 0x22, 0x00));

    TEST_ASSERT_TRUE(parsePacket(parser, row1));
    TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(parser.getAlertCount()));

    TEST_ASSERT_TRUE(parsePacket(parser, row0));
    TEST_ASSERT_EQUAL_UINT32(2, static_cast<uint32_t>(parser.getAlertCount()));
    assertContainsFrequencies(parser, 24150, 34700);

    TEST_ASSERT_TRUE(parsePacket(parser, row2));
    TEST_ASSERT_EQUAL_UINT32(2, static_cast<uint32_t>(parser.getAlertCount()));
    assertContainsFrequencies(parser, 24150, 34700);
}

void test_alert_stream_aux0_decodes_junk_photo_and_priority() {
    PacketParser parser;

    // Row 1: priority + junk + photoType=3
    // Row 2: normal signal, photoType=0
    const auto row1 = makePacket(PACKET_ID_ALERT_DATA, makeAlertPayload(1, 2, 34700, 0xB0, 0x00, 0x22, 0xC3));
    const auto row2 = makePacket(PACKET_ID_ALERT_DATA, makeAlertPayload(2, 2, 24150, 0x90, 0x80, 0x84, 0x00));

    TEST_ASSERT_TRUE(parsePacket(parser, row1));
    TEST_ASSERT_TRUE(parsePacket(parser, row2));
    TEST_ASSERT_EQUAL_UINT32(2, static_cast<uint32_t>(parser.getAlertCount()));

    const auto& alerts = parser.getAllAlerts();
    const AlertData* pri = nullptr;
    const AlertData* kBand = nullptr;
    for (size_t i = 0; i < parser.getAlertCount(); ++i) {
        if (alerts[i].frequency == 34700) {
            pri = &alerts[i];
        }
        if (alerts[i].frequency == 24150) {
            kBand = &alerts[i];
        }
    }

    TEST_ASSERT_NOT_NULL(pri);
    TEST_ASSERT_TRUE(pri->isPriority);
    TEST_ASSERT_TRUE(pri->isJunk);
    TEST_ASSERT_EQUAL_UINT8(3, pri->photoType);

    TEST_ASSERT_NOT_NULL(kBand);
    TEST_ASSERT_FALSE(kBand->isPriority);
    TEST_ASSERT_FALSE(kBand->isJunk);
    TEST_ASSERT_EQUAL_UINT8(0, kBand->photoType);

    const DisplayState& state = parser.getDisplayState();
    TEST_ASSERT_TRUE(state.hasJunkAlert);
    TEST_ASSERT_TRUE(state.hasPhotoAlert);

    const AlertData priority = parser.getPriorityAlert();
    TEST_ASSERT_EQUAL_UINT32(34700, priority.frequency);
    TEST_ASSERT_TRUE(priority.isPriority);
    TEST_ASSERT_TRUE(priority.isJunk);
    TEST_ASSERT_EQUAL_UINT8(3, priority.photoType);
}

void test_alert_stream_aux0_photo_gated_before_41037() {
    PacketParser parser;

    const auto versionPacket = makePacket(PACKET_ID_RESP_VERSION, makeVersionPayload('4', '1', '0', '3', '5'));
    TEST_ASSERT_TRUE(parsePacket(parser, versionPacket));

    const auto row = makePacket(PACKET_ID_ALERT_DATA, makeAlertPayload(1, 1, 24150, 0x90, 0x80, 0x84, 0xC3));
    TEST_ASSERT_TRUE(parsePacket(parser, row));

    const AlertData priority = parser.getPriorityAlert();
    TEST_ASSERT_TRUE(priority.isPriority);
    TEST_ASSERT_TRUE(priority.isJunk);
    TEST_ASSERT_EQUAL_UINT8(0, priority.photoType);

    const DisplayState& state = parser.getDisplayState();
    TEST_ASSERT_TRUE(state.hasJunkAlert);
    TEST_ASSERT_FALSE(state.hasPhotoAlert);
}

void test_alert_stream_aux0_junk_gated_before_41032() {
    PacketParser parser;

    const auto versionPacket = makePacket(PACKET_ID_RESP_VERSION, makeVersionPayload('4', '1', '0', '3', '1'));
    TEST_ASSERT_TRUE(parsePacket(parser, versionPacket));

    const auto row = makePacket(PACKET_ID_ALERT_DATA, makeAlertPayload(1, 1, 24150, 0x90, 0x80, 0x84, 0xC3));
    TEST_ASSERT_TRUE(parsePacket(parser, row));

    const AlertData priority = parser.getPriorityAlert();
    TEST_ASSERT_TRUE(priority.isPriority);
    TEST_ASSERT_FALSE(priority.isJunk);
    TEST_ASSERT_EQUAL_UINT8(0, priority.photoType);

    const DisplayState& state = parser.getDisplayState();
    TEST_ASSERT_FALSE(state.hasJunkAlert);
    TEST_ASSERT_FALSE(state.hasPhotoAlert);
}

void test_alert_stream_malformed_version_does_not_overwrite_gating_state() {
    PacketParser parser;

    const auto validVersion = makePacket(PACKET_ID_RESP_VERSION, makeVersionPayload('4', '1', '0', '3', '5'));
    const auto malformedVersion = makePacket(
        PACKET_ID_RESP_VERSION,
        {static_cast<uint8_t>('v'), static_cast<uint8_t>('4'), static_cast<uint8_t>('.'),
         static_cast<uint8_t>('1'), 0xFF, static_cast<uint8_t>('9'), static_cast<uint8_t>('9')});
    const auto row = makePacket(PACKET_ID_ALERT_DATA, makeAlertPayload(1, 1, 24150, 0x90, 0x80, 0x84, 0xC3));

    TEST_ASSERT_TRUE(parsePacket(parser, validVersion));
    TEST_ASSERT_TRUE(parsePacket(parser, malformedVersion));
    TEST_ASSERT_TRUE(parsePacket(parser, row));

    const DisplayState& state = parser.getDisplayState();
    TEST_ASSERT_TRUE(state.hasV1Version);
    TEST_ASSERT_EQUAL_UINT32(41035, state.v1FirmwareVersion);
    TEST_ASSERT_TRUE(state.hasJunkAlert);
    TEST_ASSERT_FALSE(state.hasPhotoAlert);

    const AlertData priority = parser.getPriorityAlert();
    TEST_ASSERT_TRUE(priority.isPriority);
    TEST_ASSERT_TRUE(priority.isJunk);
    TEST_ASSERT_EQUAL_UINT8(0, priority.photoType);
}

void test_alert_stream_ku_raw_flag_is_preserved_without_forcing_mute() {
    PacketParser parser;

    // bandArrow=0x30 => Ku bit (0x10) + front arrow (0x20).
    // raw=0xA0 lies in VR's K-band scale (Ku shares the K threshold table)
    // between the 4-bar (0x9A) and 5-bar (0xA4) cut points, i.e. VR=4 bars.
    // Compressed 0..8 → 0..6: (4*6+4)/8 = 3 bars.
    const auto row = makePacket(PACKET_ID_ALERT_DATA, makeAlertPayload(1, 1, 33800, 0xA0, 0x00, 0x30, 0x80));
    TEST_ASSERT_TRUE(parsePacket(parser, row));

    TEST_ASSERT_TRUE(parser.hasAlerts());
    TEST_ASSERT_EQUAL_UINT32(1, static_cast<uint32_t>(parser.getAlertCount()));
    const auto& alerts = parser.getAllAlerts();
    TEST_ASSERT_TRUE(alerts[0].isValid);
    TEST_ASSERT_EQUAL_UINT8(0x10, alerts[0].rawBandBits);
    TEST_ASSERT_TRUE(alerts[0].isKu);
    // Ku band must decode to BAND_KU (not BAND_NONE).
    TEST_ASSERT_EQUAL(BAND_KU, alerts[0].band);
    // Ku alerts use the K-band bargraph table so
    // they no longer report 0 bars.
    TEST_ASSERT_EQUAL_UINT8(3, alerts[0].frontStrength);
    TEST_ASSERT_EQUAL_UINT8(0, alerts[0].rearStrength);

    const DisplayState& state = parser.getDisplayState();
    TEST_ASSERT_FALSE(state.muted);
    // Ku alert must surface state.hasKuAlert so the display can re-label
    // the K cell as "Ku" while the alert is active.
    TEST_ASSERT_TRUE(state.hasKuAlert);
}

void test_alert_stream_ku_tag_requires_exact_raw_band_value() {
    PacketParser parser;

    // bandArrow=0x34 => K bit (0x04) + bit4 set + front arrow.
    // Ku tag should only be true for exact raw band bits == 0x10.
    const auto row = makePacket(PACKET_ID_ALERT_DATA, makeAlertPayload(1, 1, 24150, 0x90, 0x00, 0x34, 0x80));
    TEST_ASSERT_TRUE(parsePacket(parser, row));

    TEST_ASSERT_TRUE(parser.hasAlerts());
    TEST_ASSERT_EQUAL_UINT32(1, static_cast<uint32_t>(parser.getAlertCount()));
    const auto& alerts = parser.getAllAlerts();
    TEST_ASSERT_TRUE(alerts[0].isValid);
    TEST_ASSERT_EQUAL_UINT8(0x14, alerts[0].rawBandBits);
    TEST_ASSERT_FALSE(alerts[0].isKu);
    TEST_ASSERT_EQUAL(BAND_K, alerts[0].band);

    const DisplayState& state = parser.getDisplayState();
    TEST_ASSERT_FALSE(state.muted);
    // A non-Ku alert must NOT raise hasKuAlert.
    TEST_ASSERT_FALSE(state.hasKuAlert);
}

void test_alert_stream_without_row_priority_picks_first_usable_alert() {
    PacketParser parser;

    // Non-zero display aux0 status bits should not steer priority selection.
    const auto display = makePacket(PACKET_ID_DISPLAY_DATA, makeDisplayPayload(63, 0x03, 0x24, 0x24, 0x02));
    TEST_ASSERT_TRUE(parsePacket(parser, display));

    const auto row1 = makePacket(PACKET_ID_ALERT_DATA, makeAlertPayload(1, 2, 33800, 0x90, 0x00, 0x22, 0x00));
    const auto row2 = makePacket(PACKET_ID_ALERT_DATA, makeAlertPayload(2, 2, 33820, 0xA0, 0x00, 0x22, 0x00));

    TEST_ASSERT_TRUE(parsePacket(parser, row1));
    TEST_ASSERT_TRUE(parsePacket(parser, row2));

    const AlertData priority = parser.getPriorityAlert();
    TEST_ASSERT_EQUAL_UINT32(33800, priority.frequency);
    TEST_ASSERT_EQUAL_UINT8(0, parser.getDisplayState().v1PriorityIndex);
}

void test_alert_stream_row_priority_ignores_display_aux0_bits() {
    PacketParser parser;

    // Display aux0 contains multiple status bits set; row-level priority should still win.
    const auto display = makePacket(PACKET_ID_DISPLAY_DATA, makeDisplayPayload(63, 0x03, 0x24, 0x24, 0xF2));
    TEST_ASSERT_TRUE(parsePacket(parser, display));

    const auto row1 = makePacket(PACKET_ID_ALERT_DATA, makeAlertPayload(1, 2, 24150, 0x90, 0x00, 0x24, 0x80));
    const auto row2 = makePacket(PACKET_ID_ALERT_DATA, makeAlertPayload(2, 2, 33800, 0xA0, 0x00, 0x22, 0x00));

    TEST_ASSERT_TRUE(parsePacket(parser, row1));
    TEST_ASSERT_TRUE(parsePacket(parser, row2));

    const AlertData priority = parser.getPriorityAlert();
    TEST_ASSERT_EQUAL_UINT32(24150, priority.frequency);
    TEST_ASSERT_EQUAL_UINT8(0, parser.getDisplayState().v1PriorityIndex);
}

void test_alert_stream_unusable_row_priority_falls_back_to_first_usable() {
    PacketParser parser;

    // Row1 marked priority but has zero non-laser frequency (unusable), so row2 should win.
    const auto row1 = makePacket(PACKET_ID_ALERT_DATA, makeAlertPayload(1, 2, 0, 0x90, 0x00, 0x24, 0x80));
    const auto row2 = makePacket(PACKET_ID_ALERT_DATA, makeAlertPayload(2, 2, 33800, 0xA0, 0x00, 0x22, 0x00));

    TEST_ASSERT_TRUE(parsePacket(parser, row1));
    TEST_ASSERT_TRUE(parsePacket(parser, row2));

    const AlertData priority = parser.getPriorityAlert();
    TEST_ASSERT_EQUAL_UINT32(33800, priority.frequency);
    TEST_ASSERT_EQUAL_UINT8(1, parser.getDisplayState().v1PriorityIndex);
}

void test_alert_stream_missing_row_keeps_previous_complete_table() {
    PacketParser parser;

    // Build an initial complete table (count=2).
    const auto full1 = makePacket(PACKET_ID_ALERT_DATA, makeAlertPayload(1, 2, 34700, 0xB0, 0x00, 0x22, 0x80));
    const auto full2 = makePacket(PACKET_ID_ALERT_DATA, makeAlertPayload(2, 2, 24150, 0x90, 0x80, 0x84, 0x00));
    TEST_ASSERT_TRUE(parsePacket(parser, full1));
    TEST_ASSERT_TRUE(parsePacket(parser, full2));
    TEST_ASSERT_EQUAL_UINT32(2, static_cast<uint32_t>(parser.getAlertCount()));

    // Start next table (count=3) but provide only one row.
    const auto partial = makePacket(PACKET_ID_ALERT_DATA, makeAlertPayload(1, 3, 10525, 0x88, 0x00, 0xA8, 0x80));
    TEST_ASSERT_TRUE(parsePacket(parser, partial));

    // Parser should keep prior complete table until new table is complete.
    TEST_ASSERT_EQUAL_UINT32(2, static_cast<uint32_t>(parser.getAlertCount()));
    assertContainsFrequencies(parser, 24150, 34700);
}

void test_alert_stream_count_zero_clears_alerts() {
    PacketParser parser;

    const auto full1 = makePacket(PACKET_ID_ALERT_DATA, makeAlertPayload(1, 1, 34700, 0xB0, 0x00, 0x22, 0x80));
    TEST_ASSERT_TRUE(parsePacket(parser, full1));
    TEST_ASSERT_TRUE(parser.hasAlerts());

    // Count=0 clear row.
    const auto clear = makePacket(PACKET_ID_ALERT_DATA, std::vector<uint8_t>{0x00, 0x00});
    TEST_ASSERT_TRUE(parsePacket(parser, clear));
    TEST_ASSERT_FALSE(parser.hasAlerts());
    TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(parser.getAlertCount()));
    const DisplayState& state = parser.getDisplayState();
    TEST_ASSERT_FALSE(state.hasJunkAlert);
    TEST_ASSERT_FALSE(state.hasPhotoAlert);
}

void test_alert_stream_stale_row_not_reused_for_completion() {
    PacketParser parser;

    const auto row1 = makePacket(PACKET_ID_ALERT_DATA, makeAlertPayload(1, 2, 24150, 0x90, 0x00, 0x24, 0x80));
    const auto row2 = makePacket(PACKET_ID_ALERT_DATA, makeAlertPayload(2, 2, 33800, 0xA0, 0x00, 0x22, 0x00));

    mockMillis = 0;
    TEST_ASSERT_TRUE(parsePacket(parser, row1, mockMillis));
    TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(parser.getAlertCount()));

    // Row1 should age out and not be reused to complete count=2.
    mockMillis = 2000;
    TEST_ASSERT_TRUE(parsePacket(parser, row2, mockMillis));
    TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(parser.getAlertCount()));

    mockMillis = 2001;
    TEST_ASSERT_TRUE(parsePacket(parser, row1, mockMillis));
    TEST_ASSERT_EQUAL_UINT32(2, static_cast<uint32_t>(parser.getAlertCount()));
    assertContainsFrequencies(parser, 24150, 33800);
}

void test_alert_stream_partial_timeout_restarts_assembly() {
    PacketParser parser;

    const auto row1 = makePacket(PACKET_ID_ALERT_DATA, makeAlertPayload(1, 3, 24150, 0x90, 0x00, 0x24, 0x80));
    const auto row2 = makePacket(PACKET_ID_ALERT_DATA, makeAlertPayload(2, 3, 33800, 0xA0, 0x00, 0x22, 0x00));
    const auto row3 = makePacket(PACKET_ID_ALERT_DATA, makeAlertPayload(3, 3, 34700, 0xB0, 0x00, 0x22, 0x00));

    mockMillis = 0;
    TEST_ASSERT_TRUE(parsePacket(parser, row1, mockMillis));
    TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(parser.getAlertCount()));

    // Keep refreshing one row so it stays fresh, but not complete. The parser
    // should eventually timeout and drop the partial set.
    mockMillis = 600;
    TEST_ASSERT_TRUE(parsePacket(parser, row1, mockMillis));
    mockMillis = 1200;
    TEST_ASSERT_TRUE(parsePacket(parser, row1, mockMillis));
    mockMillis = 1900;
    TEST_ASSERT_TRUE(parsePacket(parser, row1, mockMillis));
    TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(parser.getAlertCount()));

    // After timeout reset, a fresh 3-row set should publish normally.
    mockMillis = 1901;
    TEST_ASSERT_TRUE(parsePacket(parser, row2, mockMillis));
    mockMillis = 1902;
    TEST_ASSERT_TRUE(parsePacket(parser, row1, mockMillis));
    mockMillis = 1903;
    TEST_ASSERT_TRUE(parsePacket(parser, row3, mockMillis));

    TEST_ASSERT_TRUE(parser.hasAlerts());
    TEST_ASSERT_EQUAL_UINT32(3, static_cast<uint32_t>(parser.getAlertCount()));
    assertContainsThreeFrequencies(parser, 24150, 33800, 34700);
}

void test_alert_stream_three_bogey_publishes_only_when_complete() {
    PacketParser parser;

    const auto row1 = makePacket(PACKET_ID_ALERT_DATA, makeAlertPayload(1, 3, 24150, 0x90, 0x00, 0x24, 0x80));
    const auto row2a = makePacket(PACKET_ID_ALERT_DATA, makeAlertPayload(2, 3, 33800, 0xA0, 0x00, 0x22, 0x00));
    const auto row2b = makePacket(PACKET_ID_ALERT_DATA, makeAlertPayload(2, 3, 33810, 0xA1, 0x00, 0x22, 0x00));
    const auto row3 = makePacket(PACKET_ID_ALERT_DATA, makeAlertPayload(3, 3, 34700, 0xB0, 0x00, 0x22, 0x00));

    TEST_ASSERT_TRUE(parsePacket(parser, row1));
    TEST_ASSERT_TRUE(parsePacket(parser, row2a));
    TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(parser.getAlertCount()));

    // Duplicate row index should replace row2 data but still wait for index 3.
    TEST_ASSERT_TRUE(parsePacket(parser, row2b));
    TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(parser.getAlertCount()));

    TEST_ASSERT_TRUE(parsePacket(parser, row3));
    TEST_ASSERT_TRUE(parser.hasAlerts());
    TEST_ASSERT_EQUAL_UINT32(3, static_cast<uint32_t>(parser.getAlertCount()));
    assertContainsThreeFrequencies(parser, 24150, 33810, 34700);
}

void test_alert_stream_two_bogey_same_frequency_keeps_both_rows() {
    PacketParser parser;

    const auto row1 = makePacket(PACKET_ID_ALERT_DATA, makeAlertPayload(1, 2, 33800, 0x90, 0x00, 0x24, 0x80));
    const auto row2 = makePacket(PACKET_ID_ALERT_DATA, makeAlertPayload(2, 2, 33800, 0xA0, 0x00, 0x84, 0x00));

    TEST_ASSERT_TRUE(parsePacket(parser, row1));
    TEST_ASSERT_TRUE(parsePacket(parser, row2));
    TEST_ASSERT_EQUAL_UINT32(2, static_cast<uint32_t>(parser.getAlertCount()));
    TEST_ASSERT_EQUAL_UINT32(2, static_cast<uint32_t>(countFrequency(parser, 33800)));
}

void test_alert_stream_three_bogey_same_frequency_keeps_all_rows() {
    PacketParser parser;

    const auto row1 = makePacket(PACKET_ID_ALERT_DATA, makeAlertPayload(1, 3, 33800, 0x90, 0x00, 0x24, 0x80));
    const auto row2 = makePacket(PACKET_ID_ALERT_DATA, makeAlertPayload(2, 3, 33800, 0xA0, 0x00, 0x44, 0x00));
    const auto row3 = makePacket(PACKET_ID_ALERT_DATA, makeAlertPayload(3, 3, 33800, 0xB0, 0x00, 0x84, 0x00));

    TEST_ASSERT_TRUE(parsePacket(parser, row1));
    TEST_ASSERT_TRUE(parsePacket(parser, row2));
    TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(parser.getAlertCount()));

    TEST_ASSERT_TRUE(parsePacket(parser, row3));
    TEST_ASSERT_EQUAL_UINT32(3, static_cast<uint32_t>(parser.getAlertCount()));
    TEST_ASSERT_EQUAL_UINT32(3, static_cast<uint32_t>(countFrequency(parser, 33800)));
}

void test_renderable_priority_alert_prefers_usable_priority() {
    PacketParser parser;

    const auto row1 = makePacket(PACKET_ID_ALERT_DATA, makeAlertPayload(1, 2, 34700, 0xB0, 0x00, 0x22, 0x80));
    const auto row2 = makePacket(PACKET_ID_ALERT_DATA, makeAlertPayload(2, 2, 24150, 0x90, 0x80, 0x84, 0x00));

    TEST_ASSERT_TRUE(parsePacket(parser, row1));
    TEST_ASSERT_TRUE(parsePacket(parser, row2));
    TEST_ASSERT_EQUAL_UINT32(2, static_cast<uint32_t>(parser.getAlertCount()));

    AlertData renderable;
    TEST_ASSERT_TRUE(parser.getRenderablePriorityAlert(renderable));
    TEST_ASSERT_TRUE(renderable.isValid);
    TEST_ASSERT_TRUE(renderable.band != BAND_NONE);
    TEST_ASSERT_EQUAL_UINT32(34700, renderable.frequency);
}

void test_renderable_priority_alert_returns_false_when_all_rows_unusable() {
    PacketParser parser;

    // bandArrow=0x20 carries direction-only bits and decodes to BAND_NONE.
    const auto row = makePacket(PACKET_ID_ALERT_DATA, makeAlertPayload(1, 1, 24150, 0x90, 0x80, 0x20, 0x80));
    TEST_ASSERT_TRUE(parsePacket(parser, row));
    TEST_ASSERT_EQUAL_UINT32(1, static_cast<uint32_t>(parser.getAlertCount()));

    AlertData renderable;
    TEST_ASSERT_FALSE(parser.getRenderablePriorityAlert(renderable));
}

void test_strict_alert_stream_duplicate_index_replaces_prior_row() {
    PacketParser parser;

    const auto row1a = makePacket(PACKET_ID_ALERT_DATA, makeAlertPayload(1, 2, 24150, 0x90, 0x80, 0x84, 0x00));
    const auto row1b = makePacket(PACKET_ID_ALERT_DATA, makeAlertPayload(1, 2, 24152, 0x92, 0x81, 0x84, 0x00));
    const auto row2 = makePacket(PACKET_ID_ALERT_DATA, makeAlertPayload(2, 2, 34700, 0xB0, 0x00, 0x22, 0x80));

    TEST_ASSERT_TRUE(parsePacket(parser, row1a));
    TEST_ASSERT_TRUE(parsePacket(parser, row1b));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        0,
        static_cast<uint32_t>(parser.getAlertCount()),
        "strict: parser should wait for unique row indexes before publishing a table");

    TEST_ASSERT_TRUE(parsePacket(parser, row2));
    TEST_ASSERT_EQUAL_UINT32(2, static_cast<uint32_t>(parser.getAlertCount()));
    assertContainsFrequencies(parser, 24152, 34700);
}

void test_strict_contract_parser_aux0_fields_exist() {
    const std::string types = readTextFile("src/packet_parser_types.h");
    TEST_ASSERT_FALSE_MESSAGE(types.empty(), "strict: failed to read src/packet_parser_types.h");
    TEST_ASSERT_TRUE_MESSAGE(types.find("bool isJunk") != std::string::npos,
                             "strict: AlertData must expose aux0 junk bit");
    TEST_ASSERT_TRUE_MESSAGE(types.find("uint8_t photoType") != std::string::npos,
                             "strict: AlertData must expose aux0 photo type");
    TEST_ASSERT_TRUE_MESSAGE(types.find("bool hasJunkAlert") != std::string::npos,
                             "strict: DisplayState must expose table-level junk state");
    TEST_ASSERT_TRUE_MESSAGE(types.find("bool hasPhotoAlert") != std::string::npos,
                             "strict: DisplayState must expose table-level photo state");
}

void test_strict_contract_parser_aux0_fw_gates_exist() {
    const std::string src = readTextFile("src/packet_parser_alerts.cpp");
    TEST_ASSERT_FALSE_MESSAGE(src.empty(), "strict: failed to read src/packet_parser_alerts.cpp");
    TEST_ASSERT_TRUE_MESSAGE(src.find("41032") != std::string::npos,
                             "strict: parser must gate junk aux0 bit to >= 4.1032");
    TEST_ASSERT_TRUE_MESSAGE(src.find("41037") != std::string::npos,
                             "strict: parser must gate photo aux0 bits to >= 4.1037");
}

void test_strict_contract_display_photo_fallback_exists() {
    const std::string displaySrc = readTextFile("src/display_update.cpp");
    TEST_ASSERT_FALSE_MESSAGE(displaySrc.empty(), "strict: failed to read src/display_update.cpp");
    TEST_ASSERT_TRUE_MESSAGE(displaySrc.find("state.hasPhotoAlert") != std::string::npos,
                             "strict: display update must use table-level photo fallback");
    TEST_ASSERT_TRUE_MESSAGE(displaySrc.find("priority.photoType") != std::string::npos,
                             "strict: display update must use priority photoType");
    TEST_ASSERT_TRUE_MESSAGE(displaySrc.find("alert.photoType") != std::string::npos,
                             "strict: persisted display update must use alert photoType");
}

void test_strict_contract_live_top_counter_follows_raw_v1_symbol() {
    const std::string src = readTextFile("src/display_update.cpp");
    TEST_ASSERT_FALSE_MESSAGE(src.empty(), "strict: failed to read src/display_update.cpp");
    TEST_ASSERT_TRUE_MESSAGE(src.find("char liveTopCounterChar = state.bogeyCounterChar;") != std::string::npos,
                             "strict: live top counter must start from raw V1 symbol");
    TEST_ASSERT_TRUE_MESSAGE(src.find("bool liveTopCounterDot = state.bogeyCounterDot;") != std::string::npos,
                             "strict: live top counter dot must follow raw V1 packet");
    TEST_ASSERT_TRUE_MESSAGE(src.find("static_cast<char>('0' + alertCount)") == std::string::npos,
                             "strict: live top counter must not normalize to alert count");
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_display_stream_decodes_junk_counter_char);
    RUN_TEST(test_display_stream_decodes_both_bogey_image_planes);
    RUN_TEST(test_alert_stream_out_of_order_rows_completes_table);
    RUN_TEST(test_alert_stream_duplicate_index_replaces_prior_row);
    RUN_TEST(test_alert_stream_zero_based_index_is_supported);
    RUN_TEST(test_alert_stream_ambiguous_first_row_does_not_lock_wrong_mode);
    RUN_TEST(test_alert_stream_aux0_decodes_junk_photo_and_priority);
    RUN_TEST(test_alert_stream_aux0_photo_gated_before_41037);
    RUN_TEST(test_alert_stream_aux0_junk_gated_before_41032);
    RUN_TEST(test_alert_stream_malformed_version_does_not_overwrite_gating_state);
    RUN_TEST(test_alert_stream_ku_raw_flag_is_preserved_without_forcing_mute);
    RUN_TEST(test_alert_stream_ku_tag_requires_exact_raw_band_value);
    RUN_TEST(test_alert_stream_without_row_priority_picks_first_usable_alert);
    RUN_TEST(test_alert_stream_row_priority_ignores_display_aux0_bits);
    RUN_TEST(test_alert_stream_unusable_row_priority_falls_back_to_first_usable);
    RUN_TEST(test_alert_stream_missing_row_keeps_previous_complete_table);
    RUN_TEST(test_alert_stream_count_zero_clears_alerts);
    RUN_TEST(test_alert_stream_stale_row_not_reused_for_completion);
    RUN_TEST(test_alert_stream_partial_timeout_restarts_assembly);
    RUN_TEST(test_alert_stream_three_bogey_publishes_only_when_complete);
    RUN_TEST(test_alert_stream_two_bogey_same_frequency_keeps_both_rows);
    RUN_TEST(test_alert_stream_three_bogey_same_frequency_keeps_all_rows);
    RUN_TEST(test_renderable_priority_alert_prefers_usable_priority);
    RUN_TEST(test_renderable_priority_alert_returns_false_when_all_rows_unusable);

    RUN_TEST(test_strict_alert_stream_duplicate_index_replaces_prior_row);
    RUN_TEST(test_strict_contract_parser_aux0_fields_exist);
    RUN_TEST(test_strict_contract_parser_aux0_fw_gates_exist);
    RUN_TEST(test_strict_contract_display_photo_fallback_exists);
    RUN_TEST(test_strict_contract_live_top_counter_follows_raw_v1_symbol);
    return UNITY_END();
}
