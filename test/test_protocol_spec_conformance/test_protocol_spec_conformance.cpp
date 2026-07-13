// Protocol spec conformance — the firmware's decode behavior tested against
// the machine-readable tables in docs/V1_PROTOCOL_REFERENCES.md (via the
// generated test/fixtures/protocol_spec_tables.h), NOT against expectations
// derived from reading the implementation.
//
// Motivation: signal-bar defects ship when the decode, its unit test, and its
// comments all encode the same belief about how the V1 source meter maps to
// this display. Anchoring expectations to a reviewed protocol document makes a
// belief error a single-file review problem instead of a self-consistent green
// suite.

#include <unity.h>

#include <cstdio>
#include <cstring>
#include "../mocks/Arduino.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

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

#include "../../src/v1_profiles.h"

#include "../fixtures/protocol_spec_tables.h"

namespace {

using protocol_spec::SpecBand;
using protocol_spec::SpecDirection;

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

std::vector<uint8_t> makeDisplayPayload(uint8_t barBitmap) {
    // bogey='1', no blink plane, systemStatus set so bands/bars are reported.
    return std::vector<uint8_t>{0x06, 0x00, barBitmap, 0x24, 0x24, 0x04, 0x00, 0x00, 0x00};
}

std::vector<uint8_t> makeAlertRowPayload(uint8_t bandArrow,
                                         uint8_t frontRaw,
                                         uint8_t rearRaw) {
    // Single-row table: index 1 of count 1; 34.700 GHz stand-in frequency.
    const uint16_t freqMHz = 34700;
    return std::vector<uint8_t>{
        0x11,
        static_cast<uint8_t>((freqMHz >> 8) & 0xFF),
        static_cast<uint8_t>(freqMHz & 0xFF),
        frontRaw,
        rearRaw,
        bandArrow,
        0x00,
    };
}

constexpr uint32_t kNowMs = 1000;

// The only translation between spec symbols and firmware enums. One line per
// symbol; if either enum changes shape the compiler or the assertions below
// complain immediately.
Band toBand(SpecBand band) {
    switch (band) {
        case SpecBand::LASER: return BAND_LASER;
        case SpecBand::KA:    return BAND_KA;
        case SpecBand::K:     return BAND_K;
        case SpecBand::X:     return BAND_X;
        case SpecBand::KU:    return BAND_KU;
    }
    return BAND_NONE;
}

Direction toDirection(SpecDirection direction) {
    switch (direction) {
        case SpecDirection::FRONT: return DIR_FRONT;
        case SpecDirection::SIDE:  return DIR_SIDE;
        case SpecDirection::REAR:  return DIR_REAR;
    }
    return DIR_NONE;
}

uint8_t parseAlertFrontStrength(uint8_t bandArrow, uint8_t frontRaw, Band* bandOut,
                                Direction* dirOut) {
    PacketParser parser;
    const auto packet = makePacket(PACKET_ID_ALERT_DATA,
                                   makeAlertRowPayload(bandArrow, frontRaw, 0x00));
    TEST_ASSERT_TRUE(parser.parse(packet.data(), packet.size(), kNowMs));
    TEST_ASSERT_EQUAL(1, static_cast<int>(parser.getAlertCount()));
    const AlertData alert = parser.getAllAlerts()[0];
    if (bandOut) *bandOut = alert.band;
    if (dirOut) *dirOut = alert.direction;
    return alert.frontStrength;
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_led_bitmap_decode_matches_spec_table() {
    for (const auto& c : protocol_spec::kLedBitmapBars) {
        PacketParser parser;
        const auto packet =
            makePacket(PACKET_ID_DISPLAY_DATA, makeDisplayPayload(c.bitmap));
        TEST_ASSERT_TRUE(parser.parse(packet.data(), packet.size(), kNowMs));
        char msg[48];
        std::snprintf(msg, sizeof(msg), "bitmap=0x%02X", c.bitmap);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(c.bars,
                                        parser.getDisplayState().signalBars, msg);
    }
}

void test_led_bitmap_overflow_matches_spec_sentinel() {
    const uint8_t overflowSamples[] = {0x80, 0xAA, 0x55, 0x02, 0xF0, 0x81, 0x3E};
    for (uint8_t bitmap : overflowSamples) {
        PacketParser parser;
        const auto packet =
            makePacket(PACKET_ID_DISPLAY_DATA, makeDisplayPayload(bitmap));
        TEST_ASSERT_TRUE(parser.parse(packet.data(), packet.size(), kNowMs));
        char msg[48];
        std::snprintf(msg, sizeof(msg), "overflow bitmap=0x%02X", bitmap);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(protocol_spec::kLedBitmapOverflowBars,
                                        parser.getDisplayState().signalBars, msg);
    }
}

void test_alert_band_values_match_spec_table() {
    for (const auto& c : protocol_spec::kAlertBandValues) {
        Band band = BAND_NONE;
        // Give the row a front direction so the row is a plausible alert.
        (void)parseAlertFrontStrength(static_cast<uint8_t>(c.byte | 0x20), 0x90,
                                      &band, nullptr);
        char msg[48];
        std::snprintf(msg, sizeof(msg), "band byte=0x%02X", c.byte);
        TEST_ASSERT_EQUAL_MESSAGE(toBand(c.band), band, msg);
    }
}

void test_alert_direction_bits_match_spec_table() {
    for (const auto& c : protocol_spec::kAlertDirectionBits) {
        Direction direction = DIR_NONE;
        (void)parseAlertFrontStrength(static_cast<uint8_t>(0x02 | c.bit), 0x90,
                                      nullptr, &direction);
        char msg[48];
        std::snprintf(msg, sizeof(msg), "direction bit=0x%02X", c.bit);
        TEST_ASSERT_EQUAL_MESSAGE(toDirection(c.direction), direction, msg);
    }
}

namespace {

void assertStrengthTable(uint8_t bandByte,
                         const protocol_spec::StrengthThreshold* table,
                         size_t tableLen) {
    // At each documented threshold the parser must produce the compressed
    // card value for that VR bar count; one raw unit below must produce the
    // next tier down (the table is ordered 8..1).
    for (size_t i = 0; i < tableLen; ++i) {
        const uint8_t raw = table[i].minRaw;
        const uint8_t vrBars = table[i].vrBars;
        const uint8_t expectAt = protocol_spec::kVrToCardBars[vrBars];
        char msg[64];

        std::snprintf(msg, sizeof(msg), "band=0x%02X raw=0x%02X (at threshold)",
                      bandByte, raw);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(
            expectAt,
            parseAlertFrontStrength(static_cast<uint8_t>(bandByte | 0x20), raw,
                                    nullptr, nullptr),
            msg);

        const uint8_t below = static_cast<uint8_t>(raw - 1);
        const uint8_t vrBelow = (i + 1 < tableLen)
                                    ? table[i + 1].vrBars
                                    : static_cast<uint8_t>(0);  // below 1-bar floor
        // Below the 1-bar floor (raw 0x00) VR reports 0 bars.
        const uint8_t expectBelow =
            (below == 0x00) ? protocol_spec::kVrToCardBars[0]
                            : protocol_spec::kVrToCardBars[vrBelow];
        std::snprintf(msg, sizeof(msg), "band=0x%02X raw=0x%02X (below threshold)",
                      bandByte, below);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(
            expectBelow,
            parseAlertFrontStrength(static_cast<uint8_t>(bandByte | 0x20), below,
                                    nullptr, nullptr),
            msg);
    }
}

}  // namespace

void test_ka_strength_thresholds_match_spec_table() {
    assertStrengthTable(0x02, protocol_spec::kStrengthThresholdsKa,
                        sizeof(protocol_spec::kStrengthThresholdsKa) /
                            sizeof(protocol_spec::kStrengthThresholdsKa[0]));
}

void test_x_strength_thresholds_match_spec_table() {
    assertStrengthTable(0x08, protocol_spec::kStrengthThresholdsX,
                        sizeof(protocol_spec::kStrengthThresholdsX) /
                            sizeof(protocol_spec::kStrengthThresholdsX[0]));
}

void test_k_strength_thresholds_match_spec_table() {
    assertStrengthTable(0x04, protocol_spec::kStrengthThresholdsK,
                        sizeof(protocol_spec::kStrengthThresholdsK) /
                            sizeof(protocol_spec::kStrengthThresholdsK[0]));
}

void test_ku_uses_k_band_strength_scale() {
    // VR groups Ku onto the K scale; pin one representative threshold.
    const auto& top = protocol_spec::kStrengthThresholdsK[0];
    TEST_ASSERT_EQUAL_UINT8(
        protocol_spec::kVrToCardBars[top.vrBars],
        parseAlertFrontStrength(0x10 | 0x20, top.minRaw, nullptr, nullptr));
}


// ============================================================================
// Alert-row layout + aux0 bits (spec tables: alert-row-layout, alert-aux0-bits)
// ============================================================================

void test_alert_row_layout_matches_spec_table() {
    // Pin the offsets to the documented layout (compile-time facts from the
    // fixture), then drive the parser to prove it reads them at those offsets.
    TEST_ASSERT_EQUAL_UINT8(0, protocol_spec::kAlertRowOffsetIndexCount);
    TEST_ASSERT_EQUAL_UINT8(1, protocol_spec::kAlertRowOffsetFreqMsb);
    TEST_ASSERT_EQUAL_UINT8(2, protocol_spec::kAlertRowOffsetFreqLsb);
    TEST_ASSERT_EQUAL_UINT8(3, protocol_spec::kAlertRowOffsetFrontRssi);
    TEST_ASSERT_EQUAL_UINT8(4, protocol_spec::kAlertRowOffsetRearRssi);
    TEST_ASSERT_EQUAL_UINT8(5, protocol_spec::kAlertRowOffsetBandArrow);
    TEST_ASSERT_EQUAL_UINT8(6, protocol_spec::kAlertRowOffsetAux0);

    // Frequency must be read as MSB<<8 | LSB from the documented offsets.
    PacketParser parser;
    std::vector<uint8_t> row(7, 0x00);
    row[protocol_spec::kAlertRowOffsetIndexCount] = 0x11;
    row[protocol_spec::kAlertRowOffsetFreqMsb] = 0x86;   // 0x8698 = 34456 MHz
    row[protocol_spec::kAlertRowOffsetFreqLsb] = 0x98;
    row[protocol_spec::kAlertRowOffsetFrontRssi] = 0xBA; // Ka 8-bar threshold
    row[protocol_spec::kAlertRowOffsetRearRssi] = 0x00;
    row[protocol_spec::kAlertRowOffsetBandArrow] = 0x22; // Ka | front
    const auto packet = makePacket(PACKET_ID_ALERT_DATA, row);
    TEST_ASSERT_TRUE(parser.parse(packet.data(), packet.size(), kNowMs));
    TEST_ASSERT_EQUAL(1, static_cast<int>(parser.getAlertCount()));
    const AlertData alert = parser.getAllAlerts()[0];
    TEST_ASSERT_EQUAL_UINT32(34456u, alert.frequency);
    TEST_ASSERT_EQUAL(BAND_KA, alert.band);
    TEST_ASSERT_EQUAL_UINT8(protocol_spec::kVrToCardBars[8], alert.frontStrength);
    TEST_ASSERT_EQUAL_UINT8(protocol_spec::kVrToCardBars[0], alert.rearStrength);
}

void test_alert_aux0_priority_bit_matches_spec_table() {
    PacketParser parser;
    auto row = makeAlertRowPayload(0x22, 0x90, 0x00);
    row[protocol_spec::kAlertRowOffsetAux0] = protocol_spec::kAlertAux0PriorityMask;
    const auto packet = makePacket(PACKET_ID_ALERT_DATA, row);
    TEST_ASSERT_TRUE(parser.parse(packet.data(), packet.size(), kNowMs));
    TEST_ASSERT_EQUAL(1, static_cast<int>(parser.getAlertCount()));
    TEST_ASSERT_TRUE(parser.getAllAlerts()[0].isPriority);

    // Priority bit clear -> not priority.
    PacketParser parser2;
    auto row2 = makeAlertRowPayload(0x22, 0x90, 0x00);
    row2[protocol_spec::kAlertRowOffsetAux0] = 0x00;
    const auto packet2 = makePacket(PACKET_ID_ALERT_DATA, row2);
    TEST_ASSERT_TRUE(parser2.parse(packet2.data(), packet2.size(), kNowMs));
    TEST_ASSERT_FALSE(parser2.getAllAlerts()[0].isPriority);
}

// ============================================================================
// User settings bytes (spec table: user-bytes-bit-map)
//
// Valentine's Law surface: v1_profile_push_policy flips LASER (byte0/0x08) and
// auto-push rewrites MUTE_TO_MUTE_VOLUME (byte0/0x10). A wrong row here means
// a profile push could silently disable a detection band. These tests drive
// the real V1UserSettings accessors against the documented map.
// ============================================================================

namespace {

struct UserBoolAccess {
    const char* name;
    bool (*get)(const V1UserSettings&);
    void (*set)(V1UserSettings&, bool);
};

const UserBoolAccess kUserBoolAccess[] = {
    {"X_BAND", [](const V1UserSettings& s){ return s.xBandEnabled(); }, [](V1UserSettings& s, bool v){ s.setXBandEnabled(v); }},
    {"K_BAND", [](const V1UserSettings& s){ return s.kBandEnabled(); }, [](V1UserSettings& s, bool v){ s.setKBandEnabled(v); }},
    {"KA_BAND", [](const V1UserSettings& s){ return s.kaBandEnabled(); }, [](V1UserSettings& s, bool v){ s.setKaBandEnabled(v); }},
    {"LASER", [](const V1UserSettings& s){ return s.laserEnabled(); }, [](V1UserSettings& s, bool v){ s.setLaserEnabled(v); }},
    {"MUTE_TO_MUTE_VOLUME", [](const V1UserSettings& s){ return s.muteToMuteVolume(); }, [](V1UserSettings& s, bool v){ s.setMuteToMuteVolume(v); }},
    {"BOGEY_LOCK_LOUD", [](const V1UserSettings& s){ return s.bogeyLockLoud(); }, [](V1UserSettings& s, bool v){ s.setBogeyLockLoud(v); }},
    {"MUTE_X_K_REAR", [](const V1UserSettings& s){ return s.muteXKRear(); }, [](V1UserSettings& s, bool v){ s.setMuteXKRear(v); }},
    {"KU_BAND", [](const V1UserSettings& s){ return s.kuBandEnabled(); }, [](V1UserSettings& s, bool v){ s.setKuBandEnabled(v); }},
    {"EURO_MODE", [](const V1UserSettings& s){ return s.euroMode(); }, [](V1UserSettings& s, bool v){ s.setEuroMode(v); }},
    {"K_VERIFIER", [](const V1UserSettings& s){ return s.kVerifier(); }, [](V1UserSettings& s, bool v){ s.setKVerifier(v); }},
    {"LASER_REAR", [](const V1UserSettings& s){ return s.laserRear(); }, [](V1UserSettings& s, bool v){ s.setLaserRear(v); }},
    {"CUSTOM_FREQS", [](const V1UserSettings& s){ return s.customFreqs(); }, [](V1UserSettings& s, bool v){ s.setCustomFreqs(v); }},
    {"KA_ALWAYS_PRIORITY", [](const V1UserSettings& s){ return s.kaAlwaysPriority(); }, [](V1UserSettings& s, bool v){ s.setKaAlwaysPriority(v); }},
    {"FAST_LASER_DETECT", [](const V1UserSettings& s){ return s.fastLaserDetect(); }, [](V1UserSettings& s, bool v){ s.setFastLaserDetect(v); }},
    {"STARTUP_SEQUENCE", [](const V1UserSettings& s){ return s.startupSequence(); }, [](V1UserSettings& s, bool v){ s.setStartupSequence(v); }},
    {"RESTING_DISPLAY", [](const V1UserSettings& s){ return s.restingDisplay(); }, [](V1UserSettings& s, bool v){ s.setRestingDisplay(v); }},
    {"BSM_PLUS", [](const V1UserSettings& s){ return s.bsmPlus(); }, [](V1UserSettings& s, bool v){ s.setBsmPlus(v); }},
    {"MRCT", [](const V1UserSettings& s){ return s.mrct(); }, [](V1UserSettings& s, bool v){ s.setMrct(v); }},
    {"DRIVE_SAFE_3D", [](const V1UserSettings& s){ return s.driveSafe3D(); }, [](V1UserSettings& s, bool v){ s.setDriveSafe3D(v); }},
    {"DRIVE_SAFE_3D_HD", [](const V1UserSettings& s){ return s.driveSafe3DHD(); }, [](V1UserSettings& s, bool v){ s.setDriveSafe3DHD(v); }},
    {"REDFLEX_HALO", [](const V1UserSettings& s){ return s.redflexHalo(); }, [](V1UserSettings& s, bool v){ s.setRedflexHalo(v); }},
    {"REDFLEX_NK7", [](const V1UserSettings& s){ return s.redflexNK7(); }, [](V1UserSettings& s, bool v){ s.setRedflexNK7(v); }},
    {"EKIN", [](const V1UserSettings& s){ return s.ekin(); }, [](V1UserSettings& s, bool v){ s.setEkin(v); }},
    {"PHOTO_VERIFIER", [](const V1UserSettings& s){ return s.photoVerifier(); }, [](V1UserSettings& s, bool v){ s.setPhotoVerifier(v); }},
};

struct UserFieldAccess {
    const char* name;
    uint8_t (*get)(const V1UserSettings&);
    void (*set)(V1UserSettings&, uint8_t);
};

const UserFieldAccess kUserFieldAccess[] = {
    {"KA_SENSITIVITY", [](const V1UserSettings& s){ return s.kaSensitivity(); }, [](V1UserSettings& s, uint8_t v){ s.setKaSensitivity(v); }},
    {"AUTO_MUTE", [](const V1UserSettings& s){ return s.autoMute(); }, [](V1UserSettings& s, uint8_t v){ s.setAutoMute(v); }},
    {"K_SENSITIVITY", [](const V1UserSettings& s){ return s.kSensitivity(); }, [](V1UserSettings& s, uint8_t v){ s.setKSensitivity(v); }},
    {"X_SENSITIVITY", [](const V1UserSettings& s){ return s.xSensitivity(); }, [](V1UserSettings& s, uint8_t v){ s.setXSensitivity(v); }},
};

void assertOnlyMaskedBitsChanged(const V1UserSettings& before,
                                 const V1UserSettings& after,
                                 uint8_t byteIndex, uint8_t mask,
                                 const char* name) {
    for (int b = 0; b < 6; ++b) {
        const uint8_t diff = static_cast<uint8_t>(before.bytes[b] ^ after.bytes[b]);
        const uint8_t allowed = (b == byteIndex) ? mask : 0x00;
        char msg[64];
        std::snprintf(msg, sizeof(msg), "%s touched byte%d outside its mask", name, b);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, static_cast<uint8_t>(diff & ~allowed), msg);
    }
}

}  // namespace

void test_user_bytes_bool_rows_match_spec_table() {
    size_t boolRows = 0;
    for (const auto& row : protocol_spec::kUserBytesBitMap) {
        if (row.kind == 2) continue;
        ++boolRows;
        const UserBoolAccess* acc = nullptr;
        for (const auto& a : kUserBoolAccess) {
            if (std::strcmp(a.name, row.name) == 0) { acc = &a; break; }
        }
        TEST_ASSERT_NOT_NULL_MESSAGE(acc, row.name);

        V1UserSettings s;
        acc->set(s, true);
        TEST_ASSERT_TRUE_MESSAGE(acc->get(s), row.name);
        const uint8_t bitsOn = static_cast<uint8_t>(s.bytes[row.byteIndex] & row.mask);
        if (row.kind == 0) {  // direct: bit set == ON
            TEST_ASSERT_EQUAL_UINT8_MESSAGE(row.mask, bitsOn, row.name);
        } else {              // inverted: bit clear == ON
            TEST_ASSERT_EQUAL_UINT8_MESSAGE(0x00, bitsOn, row.name);
        }

        const V1UserSettings before = s;
        acc->set(s, false);
        TEST_ASSERT_FALSE_MESSAGE(acc->get(s), row.name);
        assertOnlyMaskedBitsChanged(before, s, row.byteIndex, row.mask, row.name);
    }
    // Every bool accessor is pinned by a documented row and vice versa.
    TEST_ASSERT_EQUAL_UINT(sizeof(kUserBoolAccess) / sizeof(kUserBoolAccess[0]), boolRows);
}

void test_user_bytes_field_rows_match_spec_table() {
    size_t fieldRows = 0;
    for (const auto& row : protocol_spec::kUserBytesBitMap) {
        if (row.kind != 2) continue;
        ++fieldRows;
        const UserFieldAccess* acc = nullptr;
        for (const auto& a : kUserFieldAccess) {
            if (std::strcmp(a.name, row.name) == 0) { acc = &a; break; }
        }
        TEST_ASSERT_NOT_NULL_MESSAGE(acc, row.name);

        for (uint8_t v = 0; v <= 3; ++v) {
            V1UserSettings s;
            const V1UserSettings before = s;
            acc->set(s, v);
            TEST_ASSERT_EQUAL_UINT8_MESSAGE(v, acc->get(s), row.name);
            assertOnlyMaskedBitsChanged(before, s, row.byteIndex, row.mask, row.name);
        }
    }
    TEST_ASSERT_EQUAL_UINT(sizeof(kUserFieldAccess) / sizeof(kUserFieldAccess[0]), fieldRows);
}

void test_laser_strength_is_full_scale_regardless_of_raw() {
    const uint8_t raws[] = {0x00, 0x01, 0x7F, 0xFF};
    for (uint8_t raw : raws) {
        char msg[48];
        std::snprintf(msg, sizeof(msg), "laser raw=0x%02X", raw);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(
            protocol_spec::kVrToCardBars[protocol_spec::kLaserVrBars],
            parseAlertFrontStrength(0x01 | 0x20, raw, nullptr, nullptr), msg);
    }
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_led_bitmap_decode_matches_spec_table);
    RUN_TEST(test_led_bitmap_overflow_matches_spec_sentinel);
    RUN_TEST(test_alert_band_values_match_spec_table);
    RUN_TEST(test_alert_direction_bits_match_spec_table);
    RUN_TEST(test_ka_strength_thresholds_match_spec_table);
    RUN_TEST(test_x_strength_thresholds_match_spec_table);
    RUN_TEST(test_k_strength_thresholds_match_spec_table);
    RUN_TEST(test_ku_uses_k_band_strength_scale);
    RUN_TEST(test_laser_strength_is_full_scale_regardless_of_raw);
    RUN_TEST(test_alert_row_layout_matches_spec_table);
    RUN_TEST(test_alert_aux0_priority_bit_matches_spec_table);
    RUN_TEST(test_user_bytes_bool_rows_match_spec_table);
    RUN_TEST(test_user_bytes_field_rows_match_spec_table);
    UNITY_END();
    return 0;
}
