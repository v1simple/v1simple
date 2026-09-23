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
#define PACKET_ID_REQ_CURRENT_VOLUME 0x37
#define PACKET_ID_RESP_CURRENT_VOLUME 0x38
#define PACKET_ID_RESP_USER_BYTES 0x12
#define PACKET_ID_VERSION 0x01
#define PACKET_ID_RESP_VERSION 0x02
#define PACKET_ID_REQ_ALL_VOLUME 0x3C
#define PACKET_ID_RESP_ALL_VOLUME 0x3D
#define PACKET_ID_RESP_SWEEP_DEFINITION 0x17
#define PACKET_ID_RESP_MAX_SWEEP_INDEX 0x20
#define PACKET_ID_RESP_SWEEP_SECTIONS 0x23
#define PACKET_ID_RESP_SWEEP_WRITE_RESULT 0x21
#endif

#include "../../src/packet_parser.h"
#include "../../src/packet_parser.cpp"
#include "../../src/packet_parser_alerts.cpp"


#include <vector>

namespace {

std::vector<uint8_t> makePacket(uint8_t packetId, const std::vector<uint8_t>& payload,
                                uint8_t encodedOriginator = 0xEA, uint8_t destination = 0) {
    std::vector<uint8_t> packet;
    packet.reserve(6 + payload.size());
    packet.push_back(ESP_PACKET_START);
    if (destination == 0) {
        destination = (packetId == PACKET_ID_RESP_VERSION || packetId == PACKET_ID_RESP_USER_BYTES ||
                       packetId == PACKET_ID_RESP_CURRENT_VOLUME || packetId == PACKET_ID_RESP_ALL_VOLUME ||
                       packetId == PACKET_ID_RESP_SWEEP_DEFINITION ||
                       packetId == PACKET_ID_RESP_MAX_SWEEP_INDEX ||
                       packetId == PACKET_ID_RESP_SWEEP_SECTIONS ||
                       packetId == PACKET_ID_RESP_SWEEP_WRITE_RESULT)
                          ? 0xD6
                          : 0xD8;
    }
    packet.push_back(destination);
    packet.push_back(encodedOriginator);
    packet.push_back(packetId);
    packet.push_back(static_cast<uint8_t>(payload.size()));
    packet.insert(packet.end(), payload.begin(), payload.end());
    packet.push_back(ESP_PACKET_END);
    if (encodedOriginator == 0xEA && !payload.empty()) {
        uint8_t checksum = 0;
        for (size_t index = 0; index + 2 < packet.size(); ++index) {
            checksum = static_cast<uint8_t>(checksum + packet[index]);
        }
        packet[packet.size() - 2] = checksum;
    }
    return packet;
}

void applyEspChecksum(std::vector<uint8_t>& packet) {
    TEST_ASSERT_GREATER_OR_EQUAL_UINT(8, packet.size());
    uint8_t checksum = 0;
    for (size_t index = 0; index + 2 < packet.size(); ++index) {
        checksum = static_cast<uint8_t>(checksum + packet[index]);
    }
    packet[packet.size() - 2] = checksum;
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
    // AndroidESPLibrary2 ResponseVersion.java): 7 ASCII bytes plus checksum:
    //   [0] device letter, [1] major, [2] '.', [3] minor,
    //   [4] rev1, [5] rev2, [6] ctrl.
    return std::vector<uint8_t>{static_cast<uint8_t>('v'),
                                static_cast<uint8_t>(major),
                                static_cast<uint8_t>('.'),
                                static_cast<uint8_t>(minor),
                                static_cast<uint8_t>(rev1),
                                static_cast<uint8_t>(rev2),
                                static_cast<uint8_t>(ctrl),
                                0x00};
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
        0x03,                              // 2 bits lit -> 2 bars
        0x52,                              // Ka + side + mute
        0x42,                              // steady: Ka + side (mute not steady — flashing)
        0x04,                              // aux0: systemStatus=1 (V1 actively searching)
        0x04,                              // mode=A
        0x73);                             // main=7 mute=3
    const auto packet = makePacket(PACKET_ID_DISPLAY_DATA, payload);

    // Mute requires two consecutive display packets with the bit set.
    TEST_ASSERT_TRUE(parsePacket(parser, packet));
    TEST_ASSERT_FALSE(parser.getDisplayState().muted);

    TEST_ASSERT_TRUE(parsePacket(parser, packet));

    const DisplayState& state = parser.getDisplayState();
    TEST_ASSERT_EQUAL_UINT8(BAND_KA, state.activeBands);
    TEST_ASSERT_EQUAL_UINT8(DIR_SIDE, state.arrows);
    TEST_ASSERT_TRUE(state.muted);
    TEST_ASSERT_EQUAL_UINT8(2, state.signalBars);
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

void test_canonical_no_checksum_display_updates_volume_state() {
    PacketParser parser;
    auto payload = makeDisplayPayload(0x3F, 0x00, 0x00, 0x00, 0x04, 0x00, 0x73);
    payload.pop_back(); // E9 carries the eight display bytes without a checksum.

    TEST_ASSERT_TRUE(parsePacket(parser, makePacket(PACKET_ID_DISPLAY_DATA, payload, 0xE9)));

    const DisplayState& state = parser.getDisplayState();
    TEST_ASSERT_TRUE(state.hasVolumeData);
    TEST_ASSERT_EQUAL_UINT8(7, state.mainVolume);
    TEST_ASSERT_EQUAL_UINT8(3, state.muteVolume);
    TEST_ASSERT_TRUE(parser.displayVolumeObservation().available);
    TEST_ASSERT_EQUAL_UINT8(7, parser.displayVolumeObservation().main);
    TEST_ASSERT_EQUAL_UINT8(3, parser.displayVolumeObservation().muted);
}

void test_noncanonical_or_invalid_display_volume_never_becomes_control_state() {
    PacketParser parser;

    auto invalid = makeDisplayPayload(0x3F, 0x00, 0x00, 0x00, 0x04, 0x00, 0xA3);
    invalid.pop_back();
    TEST_ASSERT_TRUE(parsePacket(parser, makePacket(PACKET_ID_DISPLAY_DATA, invalid, 0xE9)));
    TEST_ASSERT_FALSE(parser.getDisplayState().hasVolumeData);
    TEST_ASSERT_FALSE(parser.displayVolumeObservation().available);

    const auto wrongDestination = makePacket(
        PACKET_ID_DISPLAY_DATA,
        makeDisplayPayload(0x3F, 0x00, 0x00, 0x00, 0x04, 0x00, 0x73), 0xEA, 0xD6);
    TEST_ASSERT_FALSE(parsePacket(parser, wrongDestination));
    TEST_ASSERT_FALSE(parser.getDisplayState().hasVolumeData);
    TEST_ASSERT_FALSE(parser.displayVolumeObservation().available);
}

void test_parse_display_packet_laser_keeps_led_bitmap_signal_bars() {
    PacketParser parser;
    const auto packet = makePacket(
        PACKET_ID_DISPLAY_DATA,
        makeDisplayPayload(63,
                           0x03,  // V1 source bitmap: 2 bits lit -> 2 bars
                           0x21,  // Laser + front arrow
                           0x21,
                           0x04));

    TEST_ASSERT_TRUE(parsePacket(parser, packet));

    const DisplayState& state = parser.getDisplayState();
    TEST_ASSERT_EQUAL_UINT8(BAND_LASER, state.activeBands);
    TEST_ASSERT_EQUAL_UINT8(DIR_FRONT, state.arrows);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(2, state.signalBars,
        "laser display packets must render the LED bitmap literally, not force synthetic full bars");
    TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(parser.getAlertCount()));
    TEST_ASSERT_TRUE(parser.hasAlerts());

    const AlertData directPriority = parser.getPriorityAlert();
    TEST_ASSERT_EQUAL(BAND_LASER, directPriority.band);

    AlertData priority;
    TEST_ASSERT_TRUE(parser.getRenderablePriorityAlert(priority));
    TEST_ASSERT_EQUAL(BAND_LASER, priority.band);
    TEST_ASSERT_EQUAL(DIR_FRONT, priority.direction);

    const auto emptyRadarTable = makePacket(PACKET_ID_ALERT_DATA, {0, 0, 0, 0, 0, 0, 0, 0});
    TEST_ASSERT_TRUE(parsePacket(parser, emptyRadarTable));
    TEST_ASSERT_TRUE(parser.hasAlerts());
    TEST_ASSERT_EQUAL(DIR_FRONT, parser.getPriorityAlert().direction);

    parser.resetAlertState();
    TEST_ASSERT_FALSE(parser.hasAlerts());
    TEST_ASSERT_EQUAL(BAND_NONE, parser.getDisplayState().activeBands);
    TEST_ASSERT_EQUAL(DIR_NONE, parser.getDisplayState().arrows);
}

void test_parse_display_packet_zero_volume_does_not_force_muted() {
    PacketParser parser;
    const auto packet = makePacket(
        PACKET_ID_DISPLAY_DATA,
        makeDisplayPayload(63, 0x01, 0x20, 0x20, 0x00, 0x00, 0x00));

    TEST_ASSERT_TRUE(parsePacket(parser, packet));
    // Zero volume does not imply the image1 bit-4 mute state.
    TEST_ASSERT_FALSE(parser.getDisplayState().muted);
    TEST_ASSERT_EQUAL_UINT8(0, parser.getDisplayState().mainVolume);
}

// image1 is the single bogey LED's value; image2 is its blink-off mask.
void test_parse_display_packet_captures_bogey_image2_for_blink_mask() {
    PacketParser parser;
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
    TEST_ASSERT_EQUAL_UINT8(0, static_cast<uint8_t>(state.bogeyCounterByte & ~state.bogeyCounterByte2));
}

void test_parse_display_packet_captures_blinking_bogey_indicator() {
    PacketParser parser;
    const uint8_t junkByte = 0x1E;  // segments b+c+d+e — J on a 7-seg LED
    const auto packet = makePacket(
        PACKET_ID_DISPLAY_DATA,
        makeDisplayPayload(junkByte, 0x01, 0x20, 0x20, 0x00, 0x00, 0x00, 0x00));

    TEST_ASSERT_TRUE(parsePacket(parser, packet));

    const DisplayState& state = parser.getDisplayState();
    TEST_ASSERT_EQUAL_UINT8(junkByte, state.bogeyCounterByte);
    TEST_ASSERT_EQUAL_UINT8(0x00, state.bogeyCounterByte2);
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

void test_parse_version_packet_updates_supported_volume_version() {
    PacketParser parser;
    const auto packet = makePacket(PACKET_ID_RESP_VERSION, makeVersionPayload('4', '1', '0', '2', '8'));

    TEST_ASSERT_TRUE(parsePacket(parser, packet));

    const DisplayState& state = parser.getDisplayState();
    TEST_ASSERT_TRUE(state.hasV1Version);
    TEST_ASSERT_EQUAL_UINT32(41028, state.v1FirmwareVersion);
    TEST_ASSERT_TRUE(state.supportsVolume());
}

void test_reset_v1_version_requires_a_fresh_session_response() {
    PacketParser parser;
    const auto packet = makePacket(PACKET_ID_RESP_VERSION, makeVersionPayload('4', '1', '0', '3', '9'));

    TEST_ASSERT_TRUE(parsePacket(parser, packet));
    TEST_ASSERT_TRUE(parser.getDisplayState().hasV1Version);
    TEST_ASSERT_TRUE(parsePacket(parser,
                                 makePacket(PACKET_ID_DISPLAY_DATA,
                                            makeDisplayPayload(0x77, 0x00, 0x00, 0x00, 0x84, 0x32))));
    TEST_ASSERT_TRUE(parser.getDisplayState().hasDisplayActive);
    TEST_ASSERT_TRUE(parser.getDisplayState().hasLogicMuted);

    parser.resetV1Version();
    TEST_ASSERT_FALSE(parser.getDisplayState().hasV1Version);
    TEST_ASSERT_EQUAL_UINT32(0, parser.getDisplayState().v1FirmwareVersion);
    TEST_ASSERT_FALSE(parser.getDisplayState().hasDisplayActive);
    TEST_ASSERT_FALSE(parser.getDisplayState().hasLogicMuted);
    TEST_ASSERT_FALSE(parser.getDisplayState().hasAutoMuted);
    TEST_ASSERT_FALSE(parser.getDisplayState().hasDoubleTapActive);

    TEST_ASSERT_TRUE(parsePacket(parser, packet));
    TEST_ASSERT_TRUE(parser.getDisplayState().hasV1Version);
    TEST_ASSERT_EQUAL_UINT32(41039, parser.getDisplayState().v1FirmwareVersion);
}

void test_parse_version_packet_ignores_non_digit_payload() {
    PacketParser parser;
    // Spec-compliant letter and dot, but rev1 byte is a non-digit (0xFF).
    const auto packet = makePacket(
        PACKET_ID_RESP_VERSION,
        {static_cast<uint8_t>('v'), static_cast<uint8_t>('4'), static_cast<uint8_t>('.'),
         static_cast<uint8_t>('1'), 0xFF, static_cast<uint8_t>('2'), static_cast<uint8_t>('8'), 0x00});

    TEST_ASSERT_FALSE(parsePacket(parser, packet));

    const DisplayState& state = parser.getDisplayState();
    TEST_ASSERT_FALSE(state.hasV1Version);
    TEST_ASSERT_EQUAL_UINT32(0, state.v1FirmwareVersion);
    TEST_ASSERT_FALSE(state.supportsVolume());
}

void test_parse_version_packet_ignores_short_payload() {
    PacketParser parser;
    // PL=7 carries the seven ASCII bytes but omits the required checksum.
    const auto packet = makePacket(
        PACKET_ID_RESP_VERSION,
        {static_cast<uint8_t>('v'), static_cast<uint8_t>('4'), static_cast<uint8_t>('.'),
         static_cast<uint8_t>('1'), static_cast<uint8_t>('0'), static_cast<uint8_t>('2'),
         static_cast<uint8_t>('8')});

    TEST_ASSERT_FALSE(parsePacket(parser, packet));

    const DisplayState& state = parser.getDisplayState();
    TEST_ASSERT_FALSE(state.hasV1Version);
    TEST_ASSERT_EQUAL_UINT32(0, state.v1FirmwareVersion);
}

void test_parse_version_packet_accepts_no_checksum_originator_width() {
    PacketParser parser;
    std::vector<uint8_t> payload = makeVersionPayload('4', '1', '0', '2', '8');
    payload.pop_back();
    const auto packet = makePacket(PACKET_ID_RESP_VERSION, payload, 0xE9);

    TEST_ASSERT_TRUE(parsePacket(parser, packet));
    TEST_ASSERT_TRUE(parser.getDisplayState().hasV1Version);
    TEST_ASSERT_EQUAL_UINT32(41028, parser.getDisplayState().v1FirmwareVersion);
}

void test_parse_version_packet_preserves_prior_valid_version_on_malformed_followup() {
    PacketParser parser;
    const auto validPacket = makePacket(PACKET_ID_RESP_VERSION, makeVersionPayload('4', '1', '0', '3', '5'));
    const auto malformedPacket = makePacket(
        PACKET_ID_RESP_VERSION,
        {static_cast<uint8_t>('v'), static_cast<uint8_t>('4'), static_cast<uint8_t>('.'),
         static_cast<uint8_t>('1'), 0xFF, static_cast<uint8_t>('9'), static_cast<uint8_t>('9'), 0x00});

    TEST_ASSERT_TRUE(parsePacket(parser, validPacket));
    TEST_ASSERT_FALSE(parsePacket(parser, malformedPacket));

    const DisplayState& state = parser.getDisplayState();
    TEST_ASSERT_TRUE(state.hasV1Version);
    TEST_ASSERT_EQUAL_UINT32(41035, state.v1FirmwareVersion);
}

// Outbound REQVERSION packets must not be decoded as replies, and replies from
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
         static_cast<uint8_t>('8'), 0x00});
    TEST_ASSERT_TRUE(parsePacket(parser, packet));
    const DisplayState& state = parser.getDisplayState();
    TEST_ASSERT_FALSE(state.hasV1Version);
}

void test_display_request_echoes_do_not_mutate_or_verify_display_state() {
    PacketParser parser;
    const auto darkPacket = makePacket(PACKET_ID_TURN_OFF_DISPLAY, {0x00, 0x00});
    const auto lightPacket = makePacket(PACKET_ID_TURN_ON_DISPLAY, {0x00});

    TEST_ASSERT_TRUE(parsePacket(parser, darkPacket));
    TEST_ASSERT_TRUE(parser.getDisplayState().displayOn);
    TEST_ASSERT_FALSE(parser.getDisplayState().hasDisplayOn);
    TEST_ASSERT_EQUAL_UINT32(0, parser.displayOnObservationRevision());

    TEST_ASSERT_TRUE(parsePacket(parser, lightPacket));
    TEST_ASSERT_TRUE(parser.getDisplayState().displayOn);
    TEST_ASSERT_FALSE(parser.getDisplayState().hasDisplayOn);
    TEST_ASSERT_EQUAL_UINT32(0, parser.displayOnObservationRevision());
}

void test_invalid_display_frames_cannot_mutate_render_or_control_state() {
    PacketParser parser;
    TEST_ASSERT_TRUE(parsePacket(parser,
                                 makePacket(PACKET_ID_RESP_VERSION,
                                            makeVersionPayload('4', '1', '0', '3', '9'))));
    const auto canonical = makePacket(
        PACKET_ID_DISPLAY_DATA,
        makeDisplayPayload(0x3F, 0x00, 0x00, 0x00, 0x0C, 0x04, 0x52));
    TEST_ASSERT_TRUE(parsePacket(parser, canonical));
    TEST_ASSERT_EQUAL_UINT32(1, parser.displayOnObservation().revision);
    TEST_ASSERT_TRUE(parser.displayOnObservation().available);
    TEST_ASSERT_TRUE(parser.displayOnObservation().value);
    TEST_ASSERT_EQUAL_UINT32(1, parser.modeObservation().revision);
    TEST_ASSERT_TRUE(parser.modeObservation().available);
    TEST_ASSERT_EQUAL_CHAR('A', parser.modeObservation().value);
    TEST_ASSERT_TRUE(parser.displayVolumeObservation().available);
    TEST_ASSERT_EQUAL_UINT8(5, parser.displayVolumeObservation().main);
    TEST_ASSERT_EQUAL_UINT8(2, parser.displayVolumeObservation().muted);

    auto corrupt = makePacket(
        PACKET_ID_DISPLAY_DATA,
        makeDisplayPayload(0x3F, 0x00, 0x00, 0x00, 0x04, 0x08, 0x73));
    corrupt[corrupt.size() - 2] ^= 0x01;
    TEST_ASSERT_FALSE(parsePacket(parser, corrupt));
    TEST_ASSERT_TRUE(parser.getDisplayState().displayOn);
    TEST_ASSERT_EQUAL_CHAR('A', parser.getDisplayState().modeChar);
    TEST_ASSERT_TRUE(parser.getDisplayState().hasVolumeData);
    TEST_ASSERT_EQUAL_UINT8(5, parser.getDisplayState().mainVolume);
    TEST_ASSERT_EQUAL_UINT8(2, parser.getDisplayState().muteVolume);
    TEST_ASSERT_EQUAL_UINT32(1, parser.displayOnObservation().revision);
    TEST_ASSERT_TRUE(parser.displayOnObservation().value);
    TEST_ASSERT_EQUAL_CHAR('A', parser.modeObservation().value);
    TEST_ASSERT_EQUAL_UINT8(5, parser.displayVolumeObservation().main);

    const auto wrongDestination = makePacket(
        PACKET_ID_DISPLAY_DATA,
        makeDisplayPayload(0x3F, 0x00, 0x00, 0x00, 0x04, 0x08, 0x84), 0xEA, 0xD6);
    TEST_ASSERT_FALSE(parsePacket(parser, wrongDestination));
    TEST_ASSERT_EQUAL_UINT32(1, parser.displayOnObservation().revision);
    TEST_ASSERT_EQUAL_UINT8(5, parser.getDisplayState().mainVolume);
    TEST_ASSERT_EQUAL_UINT8(2, parser.getDisplayState().muteVolume);
}

void test_current_and_all_volume_observations_remain_source_specific_and_destination_bound() {
    PacketParser parser;
    TEST_ASSERT_TRUE(parsePacket(parser,
                                 makePacket(PACKET_ID_RESP_CURRENT_VOLUME, {7, 3, 0x00})));
    TEST_ASSERT_TRUE(parser.currentVolumeObservation().available);
    TEST_ASSERT_EQUAL_UINT32(1, parser.currentVolumeObservation().revision);
    TEST_ASSERT_EQUAL_UINT8(7, parser.currentVolumeObservation().main);
    TEST_ASSERT_EQUAL_UINT8(3, parser.currentVolumeObservation().muted);

    TEST_ASSERT_TRUE(parsePacket(parser,
                                 makePacket(PACKET_ID_RESP_ALL_VOLUME, {8, 4, 5, 2, 0x00})));
    TEST_ASSERT_EQUAL_UINT32(1, parser.currentVolumeObservation().revision);
    TEST_ASSERT_EQUAL_UINT32(1, parser.allVolumeObservation().revision);
    TEST_ASSERT_EQUAL_UINT8(8, parser.allVolumeObservation().currentMain);

    TEST_ASSERT_TRUE(parsePacket(parser,
                                 makePacket(PACKET_ID_RESP_CURRENT_VOLUME, {6, 1}, 0xE9)));
    TEST_ASSERT_EQUAL_UINT32(2, parser.currentVolumeObservation().revision);
    uint8_t latestMain = 0;
    uint8_t latestMuted = 0;
    TEST_ASSERT_TRUE(parser.copyLatestCanonicalCurrentVolume(latestMain, latestMuted));
    TEST_ASSERT_EQUAL_UINT8(6, latestMain);
    TEST_ASSERT_EQUAL_UINT8(1, latestMuted);

    auto corrupt = makePacket(PACKET_ID_RESP_CURRENT_VOLUME, {9, 9, 0x00});
    corrupt[corrupt.size() - 2] ^= 0x01;
    TEST_ASSERT_FALSE(parsePacket(parser, corrupt));
    TEST_ASSERT_FALSE(parsePacket(parser,
                                  makePacket(PACKET_ID_RESP_CURRENT_VOLUME, {9, 9, 0x00}, 0xEA, 0xD8)));
    TEST_ASSERT_EQUAL_UINT32(2, parser.currentVolumeObservation().revision);
    TEST_ASSERT_EQUAL_UINT8(6, parser.currentVolumeObservation().main);
}

void test_session_settings_resets_clear_all_canonical_observations() {
    PacketParser parser;
    TEST_ASSERT_TRUE(parsePacket(parser,
                                 makePacket(PACKET_ID_RESP_VERSION,
                                            makeVersionPayload('4', '1', '0', '3', '9'))));
    TEST_ASSERT_TRUE(parsePacket(parser,
                                 makePacket(PACKET_ID_DISPLAY_DATA,
                                            makeDisplayPayload(0x3F, 0x00, 0x00, 0x00, 0x0C, 0x04, 0x52))));
    TEST_ASSERT_TRUE(parsePacket(parser,
                                 makePacket(PACKET_ID_RESP_CURRENT_VOLUME, {7, 3, 0x00})));
    TEST_ASSERT_TRUE(parsePacket(parser,
                                 makePacket(PACKET_ID_RESP_ALL_VOLUME, {7, 3, 5, 2, 0x00})));

    parser.resetModeAndDisplayState();
    parser.resetVolumeState();
    TEST_ASSERT_FALSE(parser.displayOnObservation().available);
    TEST_ASSERT_FALSE(parser.modeObservation().available);
    TEST_ASSERT_FALSE(parser.currentVolumeObservation().available);
    TEST_ASSERT_FALSE(parser.allVolumeObservation().available);
    TEST_ASSERT_FALSE(parser.displayVolumeObservation().available);
}

// Without a version-qualified aux mode, preserve the bogey-counter glyph
// fallback. This table predates the authoritative 4.1028+ aux source.
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
        // Aux1 is intentionally 0xFF; without a known version it is not safe
        // to interpret version-gated mode bits.
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

void test_mode_observation_survives_alert_glyph_until_session_reset() {
    PacketParser parser;
    TEST_ASSERT_TRUE(parsePacket(parser, makePacket(PACKET_ID_DISPLAY_DATA,
                                                    makeDisplayPayload(0x38, 0, 0, 0, 0x04))));
    TEST_ASSERT_TRUE(parser.getDisplayState().hasMode);
    TEST_ASSERT_EQUAL_CHAR('L', parser.getDisplayState().modeChar);

    TEST_ASSERT_TRUE(parsePacket(parser, makePacket(PACKET_ID_DISPLAY_DATA,
                                                    makeDisplayPayload(0x3F, 0, 0, 0, 0x04))));
    TEST_ASSERT_TRUE(parser.getDisplayState().hasMode);
    TEST_ASSERT_EQUAL_CHAR('L', parser.getDisplayState().modeChar);

    parser.resetModeAndDisplayState();
    TEST_ASSERT_FALSE(parser.getDisplayState().hasMode);
    TEST_ASSERT_EQUAL_CHAR(0, parser.getDisplayState().modeChar);
}

void test_version_qualified_aux1_mode_is_authoritative_during_alerts() {
    struct Case {
        uint8_t aux0;
        uint8_t aux1;
        char expected;
    };
    const Case cases[] = {
        {0x04, 0x04, 'A'}, {0x04, 0x08, 'l'}, {0x04, 0x0C, 'L'},
        {0x14, 0x04, 'U'}, {0x14, 0x08, 'u'},
        {0x34, 0x04, 'C'}, {0x34, 0x08, 'c'},
    };
    for (const auto& c : cases) {
        PacketParser parser;
        TEST_ASSERT_TRUE(parsePacket(parser,
                                     makePacket(PACKET_ID_RESP_VERSION, makeVersionPayload('4', '1', '0', '2', '8'))));
        // Bogey glyph is an alert count, so only auxData1 can report mode.
        TEST_ASSERT_TRUE(parsePacket(parser, makePacket(PACKET_ID_DISPLAY_DATA,
                                                        makeDisplayPayload(0x3F, 0x03, 0x22, 0x22, c.aux0, c.aux1))));
        TEST_ASSERT_TRUE(parser.getDisplayState().hasMode);
        TEST_ASSERT_EQUAL_CHAR(c.expected, parser.getDisplayState().modeChar);
    }
}

void test_version_qualified_aux1_rejects_advanced_logic_in_euro_mode() {
    PacketParser parser;
    TEST_ASSERT_TRUE(parsePacket(parser,
                                 makePacket(PACKET_ID_RESP_VERSION, makeVersionPayload('4', '1', '0', '2', '8'))));
    TEST_ASSERT_TRUE(parsePacket(parser, makePacket(PACKET_ID_DISPLAY_DATA,
                                                    makeDisplayPayload(0x77, 0x03, 0x22, 0x22, 0x04, 0x0C))));
    TEST_ASSERT_TRUE(parser.getDisplayState().hasMode);
    TEST_ASSERT_EQUAL_CHAR('L', parser.getDisplayState().modeChar);

    TEST_ASSERT_TRUE(parsePacket(parser, makePacket(PACKET_ID_DISPLAY_DATA,
                                                    makeDisplayPayload(0x3F, 0x03, 0x22, 0x22, 0x14, 0x0C))));
    TEST_ASSERT_FALSE(parser.getDisplayState().hasMode);
    TEST_ASSERT_EQUAL_CHAR(0, parser.getDisplayState().modeChar);
}

void test_aux1_mode_is_not_claimed_before_supported_firmware() {
    PacketParser parser;
    TEST_ASSERT_TRUE(parsePacket(parser,
                                 makePacket(PACKET_ID_RESP_VERSION, makeVersionPayload('4', '1', '0', '2', '7'))));
    TEST_ASSERT_TRUE(parsePacket(parser, makePacket(PACKET_ID_DISPLAY_DATA,
                                                    makeDisplayPayload(0x3F, 0x03, 0x22, 0x22, 0x04, 0x0C))));
    TEST_ASSERT_FALSE(parser.getDisplayState().hasMode);
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

void test_display_metadata_tracks_transport_and_version_qualified_bits() {
    PacketParser parser;

    // Before version qualification, only the always-defined TS holdoff bit is
    // actionable. Newer display metadata remains explicitly unavailable.
    const auto unknownVersion = makePacket(
        PACKET_ID_DISPLAY_DATA,
        makeDisplayPayload(0x77, 0x00, 0x00, 0x00, 0x86, 0x32));
    TEST_ASSERT_TRUE(parsePacket(parser, unknownVersion));
    const DisplayState& unknown = parser.getDisplayState();
    TEST_ASSERT_TRUE(unknown.timeSliceHoldoff);
    TEST_ASSERT_FALSE(unknown.hasDisplayActive);
    TEST_ASSERT_FALSE(unknown.hasLogicMuted);
    TEST_ASSERT_FALSE(unknown.hasAutoMuted);
    TEST_ASSERT_FALSE(unknown.hasDoubleTapActive);

    TEST_ASSERT_TRUE(parsePacket(parser,
                                 makePacket(PACKET_ID_RESP_VERSION,
                                            makeVersionPayload('4', '1', '0', '3', '9'))));
    const auto qualified = makePacket(
        PACKET_ID_DISPLAY_DATA,
        makeDisplayPayload(0x77, 0x00, 0x00, 0x00, 0x84, 0x32));
    TEST_ASSERT_TRUE(parsePacket(parser, qualified));
    const DisplayState& state = parser.getDisplayState();
    TEST_ASSERT_FALSE(state.timeSliceHoldoff);
    TEST_ASSERT_TRUE(state.hasDisplayActive);
    TEST_ASSERT_TRUE(state.displayActive);
    TEST_ASSERT_TRUE(state.hasLogicMuted);
    TEST_ASSERT_TRUE(state.logicMuted);
    TEST_ASSERT_TRUE(state.hasAutoMuted);
    TEST_ASSERT_TRUE(state.autoMuted);
    TEST_ASSERT_TRUE(state.hasDoubleTapActive);
    TEST_ASSERT_TRUE(state.doubleTapActive);

    parser.resetModeAndDisplayState();
    const DisplayState& reset = parser.getDisplayState();
    TEST_ASSERT_TRUE(reset.timeSliceHoldoff);
    TEST_ASSERT_FALSE(reset.hasDisplayActive);
    TEST_ASSERT_FALSE(reset.hasLogicMuted);
    TEST_ASSERT_FALSE(reset.hasAutoMuted);
    TEST_ASSERT_FALSE(reset.hasDoubleTapActive);
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

// Payloads shorter than eight bytes cannot supply aux0/aux1/aux2.
void test_parse_display_packet_rejects_short_payload() {
    PacketParser parser;
    const auto goodPkt = makePacket(PACKET_ID_DISPLAY_DATA,
                                    makeDisplayPayload(0x77, 0x03, 0x42, 0x42, 0x04));
    TEST_ASSERT_TRUE(parsePacket(parser, goodPkt));
    TEST_ASSERT_TRUE(parser.getDisplayState().systemStatus);
    TEST_ASSERT_EQUAL_UINT8(BAND_KA, parser.getDisplayState().activeBands);

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

    const DisplayState& s = parser.getDisplayState();
    TEST_ASSERT_TRUE(s.systemStatus);
    TEST_ASSERT_EQUAL_UINT8(BAND_KA, s.activeBands);
}

void test_parse_display_packet_min_payload_clears_system_status_when_aux0_zero() {
    PacketParser parser;
    const auto pkt = makePacket(PACKET_ID_DISPLAY_DATA,
                                makeDisplayPayload(0x77, 0x00, 0x00, 0x00, 0x00));
    TEST_ASSERT_TRUE(parsePacket(parser, pkt));
    const DisplayState& s = parser.getDisplayState();
    TEST_ASSERT_FALSE(s.systemStatus);
    TEST_ASSERT_FALSE(s.softMuted);
}

void test_display_packet_records_display_state_without_suppressing_alert_data() {
    PacketParser parser;
    const auto dark = makePacket(PACKET_ID_DISPLAY_DATA,
                                 makeDisplayPayload(0x77, 0x03, 0x42, 0x42, 0x04));
    TEST_ASSERT_TRUE(parsePacket(parser, dark));
    TEST_ASSERT_TRUE(parser.getDisplayState().hasDisplayOn);
    TEST_ASSERT_FALSE(parser.getDisplayState().displayOn);
    TEST_ASSERT_EQUAL_UINT8(BAND_KA, parser.getDisplayState().activeBands);

    const auto light = makePacket(PACKET_ID_DISPLAY_DATA,
                                  makeDisplayPayload(0x77, 0x03, 0x42, 0x42, 0x0C));
    TEST_ASSERT_TRUE(parsePacket(parser, light));
    TEST_ASSERT_TRUE(parser.getDisplayState().displayOn);
}

void test_parse_resp_all_volume_populates_volume_fields() {
    PacketParser parser;
    const std::vector<uint8_t> payload = {0x07, 0x02, 0x09, 0x03, 0x00};
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

// RESPALLVOLUME is authoritative over the display packet's aux2 fallback.
void test_resp_all_volume_overrides_display_aux2_inference() {
    PacketParser parser;
    const auto disp = makePacket(PACKET_ID_DISPLAY_DATA,
                                 makeDisplayPayload(0x77, 0x00, 0x20, 0x20, 0x04, 0x00, 0x73));
    TEST_ASSERT_TRUE(parsePacket(parser, disp));
    TEST_ASSERT_EQUAL_UINT8(7, parser.getDisplayState().mainVolume);
    TEST_ASSERT_EQUAL_UINT8(3, parser.getDisplayState().muteVolume);
    TEST_ASSERT_FALSE(parser.getDisplayState().hasSavedVolume);

    const auto vol = makePacket(PACKET_ID_RESP_ALL_VOLUME,
                                std::vector<uint8_t>{0x05, 0x01, 0x08, 0x04, 0x00});
    TEST_ASSERT_TRUE(parsePacket(parser, vol));
    TEST_ASSERT_EQUAL_UINT8(5, parser.getDisplayState().mainVolume);
    TEST_ASSERT_EQUAL_UINT8(1, parser.getDisplayState().muteVolume);
    TEST_ASSERT_EQUAL_UINT8(8, parser.getDisplayState().savedMainVolume);
    TEST_ASSERT_EQUAL_UINT8(4, parser.getDisplayState().savedMuteVolume);
    TEST_ASSERT_TRUE(parser.getDisplayState().hasSavedVolume);
}

void test_parse_resp_all_volume_rejects_short_payload() {
    PacketParser parser;
    // PL=4 omits the checksum; it must not populate saved-volume fields.
    const auto pkt = makePacket(PACKET_ID_RESP_ALL_VOLUME,
                                std::vector<uint8_t>{0x07, 0x02, 0x09, 0x03});
    TEST_ASSERT_FALSE(parsePacket(parser, pkt));
    const DisplayState& s = parser.getDisplayState();
    TEST_ASSERT_FALSE(s.hasSavedVolume);
    TEST_ASSERT_EQUAL_UINT8(0, s.savedMainVolume);
    TEST_ASSERT_EQUAL_UINT8(0, s.savedMuteVolume);
}

void test_parse_resp_all_volume_accepts_no_checksum_originator_width() {
    PacketParser parser;
    const auto pkt = makePacket(PACKET_ID_RESP_ALL_VOLUME,
                                std::vector<uint8_t>{0x07, 0x02, 0x09, 0x03}, 0xE9);
    TEST_ASSERT_TRUE(parsePacket(parser, pkt));
    const DisplayState& state = parser.getDisplayState();
    TEST_ASSERT_TRUE(state.hasSavedVolume);
    TEST_ASSERT_EQUAL_UINT8(9, state.savedMainVolume);
    TEST_ASSERT_EQUAL_UINT8(3, state.savedMuteVolume);
}

void test_canonical_resp_all_volume_updates_state() {
#ifndef ARDUINO
    PacketParser parser;
    const uint8_t packet[] = {
        0xAA, 0xD6, 0xEA, 0x3D, 0x05, 0x07, 0x02, 0x04, 0x00, 0xB9, 0xAB,
    };

    TEST_ASSERT_TRUE(parser.parse(packet, sizeof(packet), kDefaultParseNowMs));
    const DisplayState& state = parser.getDisplayState();
    TEST_ASSERT_EQUAL_UINT8(7, state.mainVolume);
    TEST_ASSERT_EQUAL_UINT8(2, state.muteVolume);
    TEST_ASSERT_EQUAL_UINT8(4, state.savedMainVolume);
    TEST_ASSERT_EQUAL_UINT8(0, state.savedMuteVolume);
#endif
}

void test_noncanonical_resp_all_volume_shapes_do_not_update_state() {
#ifndef ARDUINO
    const uint8_t shortPacket[] = {
        0xAA, 0xD6, 0xEA, 0x3D, 0x04, 0x01, 0x02, 0x03, 0x04, 0xAB,
    };
    const uint8_t overlongPacket[] = {
        0xAA, 0xD6, 0xEA, 0x3D, 0x06, 0x02, 0x03, 0x04, 0x05, 0x00, 0xBB, 0xAB,
    };
    const uint8_t declaredShortPacket[] = {
        0xAA, 0xD6, 0xEA, 0x3D, 0x04, 0x03, 0x04, 0x05, 0x06, 0xBD, 0xAB,
    };
    const uint8_t declaredLongPacket[] = {
        0xAA, 0xD6, 0xEA, 0x3D, 0x06, 0x04, 0x05, 0x06, 0x07, 0xC3, 0xAB,
    };

    const struct {
        const uint8_t* packet;
        size_t size;
    } cases[] = {{shortPacket, sizeof(shortPacket)},
                 {overlongPacket, sizeof(overlongPacket)},
                 {declaredShortPacket, sizeof(declaredShortPacket)},
                 {declaredLongPacket, sizeof(declaredLongPacket)}};
    for (const auto& c : cases) {
        PacketParser parser;
        TEST_ASSERT_FALSE(parser.parse(c.packet, c.size, kDefaultParseNowMs));
        TEST_ASSERT_FALSE(parser.getDisplayState().hasVolumeData);
        TEST_ASSERT_FALSE(parser.getDisplayState().hasSavedVolume);
    }
#endif
}

void test_resp_all_volume_rejects_out_of_range_fields_atomically() {
    PacketParser parser;
    auto prior = makePacket(PACKET_ID_RESP_ALL_VOLUME,
                            std::vector<uint8_t>{0x01, 0x02, 0x03, 0x04, 0x00});
    applyEspChecksum(prior);
    TEST_ASSERT_TRUE(parsePacket(parser, prior));

    const uint8_t invalidValues[] = {0x10, 0x17};
    for (uint8_t invalid : invalidValues) {
        for (size_t position = 0; position < 4; ++position) {
            std::vector<uint8_t> payload = {0x05, 0x06, 0x07, 0x08, 0x00};
            payload[position] = invalid;
            auto malformed = makePacket(PACKET_ID_RESP_ALL_VOLUME, payload);
            applyEspChecksum(malformed); // Framing/checksum valid; only the full-byte value is invalid.

            TEST_ASSERT_FALSE(parsePacket(parser, malformed));
            const DisplayState& state = parser.getDisplayState();
            TEST_ASSERT_TRUE(state.hasVolumeData);
            TEST_ASSERT_TRUE(state.hasSavedVolume);
            TEST_ASSERT_EQUAL_UINT8(0x01, state.mainVolume);
            TEST_ASSERT_EQUAL_UINT8(0x02, state.muteVolume);
            TEST_ASSERT_EQUAL_UINT8(0x03, state.savedMainVolume);
            TEST_ASSERT_EQUAL_UINT8(0x04, state.savedMuteVolume);
        }
    }
}

void test_canonical_width_wrong_id_does_not_update_volume() {
#ifndef ARDUINO
    PacketParser parser;
    const uint8_t packet[] = {
        0xAA, 0xD6, 0xEA, 0x3C, 0x05, 0x07, 0x02, 0x04, 0x00, 0xB8, 0xAB,
    };

    TEST_ASSERT_TRUE(parser.parse(packet, sizeof(packet), kDefaultParseNowMs));
    TEST_ASSERT_FALSE(parser.getDisplayState().hasSavedVolume);
#endif
}

// The bar graph byte decodes per ESP Specification 3.015 Table 9.1 (p.34),
// which defines nine values for LED counts 0..8 and is generation-neutral.
// Fidelity for what the V1 says; fail-loud for what it cannot have said.
//
// Inside the table the decode is exact. Outside it the byte is not a strength
// the V1 could have sent, so it takes VR getNumberOfLEDS()'s default-branch
// value rather than a literal count that would understate a live threat.
void test_decode_signal_bars_renders_valid_bitmaps_literally_and_fails_loud() {
    PacketParser parser;
    // image1 = front arrow only, aux0 systemStatus=1 so bands/bars are reported.
    struct Case { uint8_t bitmap; uint8_t expected; };
    const Case cases[] = {
        // Table 9.1, all nine rows. V1Simple's six-slot meter is a renderer
        // constraint, not a protocol limit — see drawVerticalSignalBars().
        {0x00, 0},
        {0x01, 1}, {0x03, 2}, {0x07, 3}, {0x0F, 4},
        {0x1F, 5}, {0x3F, 6},  // Gen2 full scale
        {0x7F, 7}, {0xFF, 8},  // Table 9.1's top two rows
        // Everything else is absent from the table and takes the sentinel. A
        // literal read would understate several of these, which is the failure
        // this path must not have.
        {0x80, 8},  // single high bit — a literal read would say 1
        {0x02, 8},  // single mid bit
        {0xAA, 8}, {0x55, 8},  // alternating — a literal read would say 4
        {0xF0, 8},  // upper nibble
        {0x3E, 8},  // contiguous but not from LSB
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


// Parser-level invariant: no byte on the wire can produce an LED count above
// Table 9.1's ceiling of eight. The tighter six-slot ceiling is a V1Simple
// renderer property and is asserted there, not here — see
// test_display_rendering_bands.
void test_all_v1_bitmaps_are_bounded_by_eight_protocol_leds() {
    for (int value = 0; value <= 255; ++value) {
        PacketParser parser;
        const auto packet = makePacket(
            PACKET_ID_DISPLAY_DATA,
            makeDisplayPayload(63, static_cast<uint8_t>(value), 0x22, 0x22, 0x04, 0x00, 0x00));
        TEST_ASSERT_TRUE(parsePacket(parser, packet));
        char msg[64];
        std::snprintf(msg, sizeof(msg), "bitmap=0x%02X exceeded eight LEDs", value);
        TEST_ASSERT_LESS_OR_EQUAL_UINT8_MESSAGE(8, parser.getDisplayState().signalBars, msg);
    }
}

void test_bluetooth_indicator_requires_supported_version_and_accepts_both_blink_images() {
    PacketParser parser;
    auto display = makePacket(PACKET_ID_DISPLAY_DATA,
                              makeDisplayPayload(63, 0, 0, 0, 0, 0x40, 0));
    TEST_ASSERT_TRUE(parser.parse(display.data(), display.size(), 1000, 1));
    TEST_ASSERT_FALSE(parser.bluetoothIndicatorObservation().available);

    const auto oldVersion = makePacket(PACKET_ID_RESP_VERSION, makeVersionPayload('4', '1', '0', '1', '7'));
    TEST_ASSERT_TRUE(parser.parse(oldVersion.data(), oldVersion.size(), 1001, 2));
    TEST_ASSERT_TRUE(parser.parse(display.data(), display.size(), 1002, 3));
    TEST_ASSERT_FALSE(parser.bluetoothIndicatorObservation().available);

    const auto supported = makePacket(PACKET_ID_RESP_VERSION, makeVersionPayload('4', '1', '0', '1', '8'));
    TEST_ASSERT_TRUE(parser.parse(supported.data(), supported.size(), 1003, 4));
    TEST_ASSERT_TRUE(parser.parse(display.data(), display.size(), 1004, 5));
    TEST_ASSERT_EQUAL_INT(V1BluetoothIndicatorState::Blinking,
                          parser.bluetoothIndicatorObservation().state);

    display = makePacket(PACKET_ID_DISPLAY_DATA,
                         makeDisplayPayload(63, 0, 0, 0, 0, 0x80, 0));
    TEST_ASSERT_TRUE(parser.parse(display.data(), display.size(), 1005, 6));
    TEST_ASSERT_EQUAL_INT(V1BluetoothIndicatorState::Blinking,
                          parser.bluetoothIndicatorObservation().state);

    display = makePacket(PACKET_ID_DISPLAY_DATA,
                         makeDisplayPayload(63, 0, 0, 0, 0, 0xC0, 0));
    TEST_ASSERT_TRUE(parser.parse(display.data(), display.size(), 1006, 7));
    TEST_ASSERT_EQUAL_INT(V1BluetoothIndicatorState::On,
                          parser.bluetoothIndicatorObservation().state);
}

void test_sweep_collectors_poison_conflicts_and_require_exact_max_set() {
    PacketParser parser;
    const auto sections = makePacket(PACKET_ID_RESP_SWEEP_SECTIONS,
                                     {0x12, 0x61, 0xA8, 0x5D, 0xC0,
                                      0x22, 0x8C, 0xA0, 0x80, 0xE8, 0});
    TEST_ASSERT_TRUE(parser.parse(sections.data(), sections.size(), 1000, 1));
    TEST_ASSERT_TRUE(parser.sweepSectionsObservation().complete);
    TEST_ASSERT_EQUAL_UINT8(0, parser.sweepSectionsObservation().sections[0].index);
    TEST_ASSERT_EQUAL_UINT8(1, parser.sweepSectionsObservation().sections[1].index);

    parser.resetSweepSectionsObservation();
    const auto section1Only = makePacket(PACKET_ID_RESP_SWEEP_SECTIONS,
                                         {0x12, 0x61, 0xA8, 0x5D, 0xC0, 0});
    const auto section2Only = makePacket(PACKET_ID_RESP_SWEEP_SECTIONS,
                                         {0x22, 0x8C, 0xA0, 0x80, 0xE8, 0});
    TEST_ASSERT_TRUE(parser.parse(section1Only.data(), section1Only.size(), 1001, 2));
    TEST_ASSERT_FALSE(parser.sweepSectionsObservation().complete);
    TEST_ASSERT_EQUAL_HEX16(0x01, parser.sweepSectionsObservation().presentMask);
    TEST_ASSERT_TRUE(parser.parse(section2Only.data(), section2Only.size(), 1002, 3));
    TEST_ASSERT_TRUE(parser.sweepSectionsObservation().complete);
    TEST_ASSERT_EQUAL_HEX16(0x03, parser.sweepSectionsObservation().presentMask);

    parser.resetSweepSectionsObservation();
    const auto mixedNullSections = makePacket(
        PACKET_ID_RESP_SWEEP_SECTIONS,
        {0x13, 0x61, 0xA8, 0x5D, 0xC0,
         0x23, 0x00, 0x00, 0x00, 0x00,
         0x33, 0x8C, 0xA0, 0x80, 0xE8, 0});
    TEST_ASSERT_TRUE(parser.parse(mixedNullSections.data(), mixedNullSections.size(), 1003, 4));
    TEST_ASSERT_TRUE(parser.sweepSectionsObservation().complete);
    TEST_ASSERT_EQUAL_UINT16(0, parser.sweepSectionsObservation().sections[1].lowerMHz);
    TEST_ASSERT_EQUAL_UINT16(0, parser.sweepSectionsObservation().sections[1].upperMHz);

    const auto countConflict = makePacket(PACKET_ID_RESP_SWEEP_SECTIONS,
                                          {0x13, 0x61, 0xA8, 0x5D, 0xC0, 0});
    TEST_ASSERT_FALSE(parser.parse(countConflict.data(), countConflict.size(), 1004, 5));
    TEST_ASSERT_TRUE(parser.sweepSectionsObservation().poisoned); // duplicate selector, even identical
    TEST_ASSERT_FALSE(parser.sweepSectionsObservation().complete);
    parser.resetSweepSectionsObservation();

    const auto section1 = makePacket(PACKET_ID_RESP_SWEEP_SECTIONS,
                                     {0x12, 0x61, 0xA8, 0x5D, 0xC0, 0});
    const auto section1Duplicate = makePacket(PACKET_ID_RESP_SWEEP_SECTIONS,
                                              {0x12, 0x61, 0xA8, 0x5D, 0xC0, 0});
    TEST_ASSERT_TRUE(parser.parse(section1.data(), section1.size(), 1005, 6));
    TEST_ASSERT_TRUE(parser.sweepSectionsObservation().available);
    TEST_ASSERT_FALSE(parser.sweepSectionsObservation().complete);
    TEST_ASSERT_FALSE(parser.parse(section1Duplicate.data(), section1Duplicate.size(), 1006, 7));
    TEST_ASSERT_TRUE(parser.sweepSectionsObservation().poisoned);

    parser.resetSweepSectionsObservation();
    const auto zeroBasedSection = makePacket(PACKET_ID_RESP_SWEEP_SECTIONS,
                                             {0x02, 0x61, 0xA8, 0x5D, 0xC0, 0});
    TEST_ASSERT_FALSE(parser.parse(zeroBasedSection.data(), zeroBasedSection.size(), 1007, 8));
    TEST_ASSERT_TRUE(parser.sweepSectionsObservation().poisoned);

    parser.resetSweepDefinitionsObservation();
    parser.resetSweepMaxObservation();
    const auto definition0 = makePacket(PACKET_ID_RESP_SWEEP_DEFINITION,
                                        {0x80, 0x5E, 0x56, 0x5D, 0xF2, 0});
    const auto definition0Conflict = makePacket(PACKET_ID_RESP_SWEEP_DEFINITION,
                                                {0x80, 0x5E, 0x57, 0x5D, 0xF2, 0});
    TEST_ASSERT_TRUE(parser.parse(definition0.data(), definition0.size(), 1007, 8));
    TEST_ASSERT_FALSE(parser.parse(definition0Conflict.data(), definition0Conflict.size(), 1008, 9));
    TEST_ASSERT_TRUE(parser.sweepDefinitionsObservation().poisoned);

    parser.resetSweepDefinitionsObservation();
    parser.resetSweepMaxObservation();
    const auto extra = makePacket(PACKET_ID_RESP_SWEEP_DEFINITION,
                                  {0x82, 0, 0, 0, 0, 0});
    const auto maxOne = makePacket(PACKET_ID_RESP_MAX_SWEEP_INDEX, {0x01, 0});
    TEST_ASSERT_TRUE(parser.parse(extra.data(), extra.size(), 1009, 10));
    TEST_ASSERT_TRUE(parser.parse(maxOne.data(), maxOne.size(), 1010, 11));
    TEST_ASSERT_TRUE(parser.sweepDefinitionsObservation().poisoned);

    parser.resetSweepDefinitionsObservation();
    TEST_ASSERT_TRUE(parser.parse(maxOne.data(), maxOne.size(), 1011, 12));
    TEST_ASSERT_FALSE(parser.parse(extra.data(), extra.size(), 1012, 13));
    TEST_ASSERT_TRUE(parser.sweepDefinitionsObservation().poisoned);

    parser.resetSweepMaxObservation();
    const auto maxTwo = makePacket(PACKET_ID_RESP_MAX_SWEEP_INDEX, {0x02, 0});
    TEST_ASSERT_TRUE(parser.parse(maxOne.data(), maxOne.size(), 1013, 14));
    TEST_ASSERT_TRUE(parser.parse(maxOne.data(), maxOne.size(), 1014, 15));
    TEST_ASSERT_FALSE(parser.sweepMaxObservation().poisoned);
    TEST_ASSERT_TRUE(parser.sweepMaxObservation().available);
    TEST_ASSERT_EQUAL_UINT32(15, parser.sweepMaxObservation().ingressSequence);
    TEST_ASSERT_FALSE(parser.parse(maxTwo.data(), maxTwo.size(), 1015, 16));
    TEST_ASSERT_TRUE(parser.sweepMaxObservation().poisoned);
    TEST_ASSERT_FALSE(parser.sweepMaxObservation().available);
    TEST_ASSERT_EQUAL_UINT8(1, parser.sweepMaxObservation().maxIndex);
    TEST_ASSERT_FALSE(parser.parse(maxOne.data(), maxOne.size(), 1016, 17));
}

void test_sweep_responses_require_canonical_destination_checksum_and_index_bits() {
    PacketParser parser;
    const auto max63 = makePacket(PACKET_ID_RESP_MAX_SWEEP_INDEX, {0x3F, 0});
    const auto max64 = makePacket(PACKET_ID_RESP_MAX_SWEEP_INDEX, {0x40, 0});
    TEST_ASSERT_TRUE(parser.parse(max63.data(), max63.size(), 999, 1));
    TEST_ASSERT_EQUAL_UINT8(63, parser.sweepMaxObservation().maxIndex);
    TEST_ASSERT_FALSE(parser.parse(max64.data(), max64.size(), 1000, 2));
    TEST_ASSERT_TRUE(parser.sweepMaxObservation().poisoned);
    TEST_ASSERT_FALSE(parser.sweepMaxObservation().available);
    TEST_ASSERT_EQUAL_UINT8(63, parser.sweepMaxObservation().maxIndex);

    auto definition = makePacket(PACKET_ID_RESP_SWEEP_DEFINITION,
                                 {0x80, 0x5E, 0x56, 0x5D, 0xF2, 0}, 0xEA, 0xD8);
    TEST_ASSERT_FALSE(parser.parse(definition.data(), definition.size(), 1001, 3));
    definition = makePacket(PACKET_ID_RESP_SWEEP_DEFINITION,
                            {0x40, 0x5E, 0x56, 0x5D, 0xF2, 0});
    TEST_ASSERT_FALSE(parser.parse(definition.data(), definition.size(), 1002, 4));
    TEST_ASSERT_TRUE(parser.sweepDefinitionsObservation().poisoned);
    parser.resetSweepDefinitionsObservation();
    definition = makePacket(PACKET_ID_RESP_SWEEP_DEFINITION,
                            {0x80, 0x5E, 0x56, 0x5D, 0xF2, 0});
    TEST_ASSERT_TRUE(parser.parse(definition.data(), definition.size(), 1003, 5));
    TEST_ASSERT_EQUAL_UINT8(0, parser.sweepDefinitionsObservation().definitions[0].index);
    parser.resetSweepDefinitionsObservation();
    // Bit 7 is reserved in the field description; vendor decoders mask it,
    // so the equivalent clear-bit response remains compatible.
    definition = makePacket(PACKET_ID_RESP_SWEEP_DEFINITION,
                            {0x00, 0x5E, 0x56, 0x5D, 0xF2, 0});
    TEST_ASSERT_TRUE(parser.parse(definition.data(), definition.size(), 1004, 6));
    parser.resetSweepDefinitionsObservation();
    definition = makePacket(PACKET_ID_RESP_SWEEP_DEFINITION,
                            {0x80, 0x5E, 0x56, 0x5D, 0xF2, 0});
    definition[definition.size() - 2] ^= 0x01;
    TEST_ASSERT_FALSE(parser.parse(definition.data(), definition.size(), 1005, 7));
    TEST_ASSERT_EQUAL_UINT64(0, parser.sweepDefinitionsObservation().presentMask);
    TEST_ASSERT_FALSE(parser.sweepDefinitionsObservation().poisoned);

    parser.resetSweepDefinitionsObservation();
    const auto halfZero = makePacket(PACKET_ID_RESP_SWEEP_DEFINITION,
                                     {0x80, 0x5E, 0x56, 0x00, 0x00, 0});
    TEST_ASSERT_FALSE(parser.parse(halfZero.data(), halfZero.size(), 1006, 8));
    TEST_ASSERT_TRUE(parser.sweepDefinitionsObservation().poisoned);
    TEST_ASSERT_EQUAL_UINT64(0, parser.sweepDefinitionsObservation().presentMask);
    TEST_ASSERT_FALSE(parser.parse(definition.data(), definition.size(), 1007, 9));

    parser.resetSweepDefinitionsObservation();
    const auto reversed = makePacket(PACKET_ID_RESP_SWEEP_DEFINITION,
                                     {0x80, 0x5D, 0xF2, 0x5E, 0x56, 0});
    TEST_ASSERT_FALSE(parser.parse(reversed.data(), reversed.size(), 1008, 10));
    TEST_ASSERT_TRUE(parser.sweepDefinitionsObservation().poisoned);

    parser.resetSweepDefinitionsObservation();
    parser.resetSweepMaxObservation();
    const auto maxZero = makePacket(PACKET_ID_RESP_MAX_SWEEP_INDEX, {0x00, 0});
    TEST_ASSERT_TRUE(parser.parse(maxZero.data(), maxZero.size(), 1009, 11));
    const auto valid = makePacket(PACKET_ID_RESP_SWEEP_DEFINITION,
                                  {0x80, 0x5E, 0x56, 0x5D, 0xF2, 0});
    TEST_ASSERT_TRUE(parser.parse(valid.data(), valid.size(), 1010, 12));
    TEST_ASSERT_EQUAL_UINT64(1, parser.sweepDefinitionsObservation().presentMask);
    TEST_ASSERT_FALSE(parser.parse(reversed.data(), reversed.size(), 1011, 13));
    TEST_ASSERT_TRUE(parser.sweepDefinitionsObservation().poisoned);
}

void test_v1_flow_control_packets_require_their_specified_destinations() {
    PacketParser parser;

    auto rejected = makePacket(PACKET_ID_RESP_REQUEST_NOT_PROCESSED, {PACKET_ID_REQ_MAX_SWEEP_INDEX, 0},
                               0xEA, 0xD6);
    TEST_ASSERT_TRUE(parser.parse(rejected.data(), rejected.size(), 1000, 1));

    auto busy = makePacket(PACKET_ID_INF_V1_BUSY,
                           {PACKET_ID_REQ_MAX_SWEEP_INDEX, PACKET_ID_REQ_ALL_SWEEP_DEFINITIONS, 0},
                           0xEA, 0xD8);
    TEST_ASSERT_TRUE(parser.parse(busy.data(), busy.size(), 1001, 2));

    auto noChecksumBusy = makePacket(PACKET_ID_INF_V1_BUSY,
                                     {PACKET_ID_REQ_SWEEP_SECTIONS}, 0xE9, 0xD8);
    TEST_ASSERT_TRUE(parser.parse(noChecksumBusy.data(), noChecksumBusy.size(), 1002, 3));

    // ESP 3.016 p40: infV1Busy is always General Broadcast (D8), even
    // though respRequestNotProcessed is a targeted reply (D6).
    const auto targetedBusy = makePacket(PACKET_ID_INF_V1_BUSY,
                                         {PACKET_ID_REQ_MAX_SWEEP_INDEX, 0}, 0xEA, 0xD6);
    TEST_ASSERT_FALSE(parser.parse(targetedBusy.data(), targetedBusy.size(), 1002, 4));
    const auto fiveBusy = makePacket(PACKET_ID_INF_V1_BUSY,
                                     {1, 2, 3, 4, 5, 0}, 0xEA, 0xD8);
    TEST_ASSERT_TRUE(parser.parse(fiveBusy.data(), fiveBusy.size(), 1002, 5));

    rejected[1] = 0xD8;
    applyEspChecksum(rejected);
    TEST_ASSERT_FALSE(parser.parse(rejected.data(), rejected.size(), 1003, 4));

    busy = makePacket(PACKET_ID_INF_V1_BUSY,
                      {PACKET_ID_REQ_MAX_SWEEP_INDEX, 0}, 0xEA, 0xD8);
    busy[busy.size() - 2] ^= 0x01;
    TEST_ASSERT_FALSE(parser.parse(busy.data(), busy.size(), 1004, 5));

    const auto emptyBusy = makePacket(PACKET_ID_INF_V1_BUSY, {0}, 0xEA, 0xD8);
    TEST_ASSERT_FALSE(parser.parse(emptyBusy.data(), emptyBusy.size(), 1005, 6));
    const auto tooManyBusy = makePacket(PACKET_ID_INF_V1_BUSY,
                                        {1, 2, 3, 4, 5, 6, 0}, 0xEA, 0xD8);
    TEST_ASSERT_FALSE(parser.parse(tooManyBusy.data(), tooManyBusy.size(), 1006, 7));
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_parse_display_packet_updates_render_state);
    RUN_TEST(test_canonical_no_checksum_display_updates_volume_state);
    RUN_TEST(test_noncanonical_or_invalid_display_volume_never_becomes_control_state);
    RUN_TEST(test_parse_display_packet_laser_keeps_led_bitmap_signal_bars);
    RUN_TEST(test_parse_display_packet_zero_volume_does_not_force_muted);
    RUN_TEST(test_parse_display_packet_captures_bogey_image2_for_blink_mask);
    RUN_TEST(test_parse_display_packet_captures_blinking_bogey_indicator);
    RUN_TEST(test_parse_packet_rejects_six_byte_frame);
    RUN_TEST(test_parse_packet_rejects_seven_byte_frame);
    RUN_TEST(test_parse_packet_rejects_bad_framing);
    RUN_TEST(test_parse_version_packet_updates_supported_volume_version);
    RUN_TEST(test_reset_v1_version_requires_a_fresh_session_response);
    RUN_TEST(test_parse_version_packet_ignores_non_digit_payload);
    RUN_TEST(test_parse_version_packet_ignores_short_payload);
    RUN_TEST(test_parse_version_packet_accepts_no_checksum_originator_width);
    RUN_TEST(test_parse_version_packet_preserves_prior_valid_version_on_malformed_followup);
    RUN_TEST(test_parse_version_packet_rejects_request_id);
    RUN_TEST(test_parse_version_packet_ignores_non_v1_device_letter);
    RUN_TEST(test_display_request_echoes_do_not_mutate_or_verify_display_state);
    RUN_TEST(test_invalid_display_frames_cannot_mutate_render_or_control_state);
    RUN_TEST(test_current_and_all_volume_observations_remain_source_specific_and_destination_bound);
    RUN_TEST(test_session_settings_resets_clear_all_canonical_observations);
    RUN_TEST(test_parse_display_packet_decodes_mode_from_bogey_glyph);
    RUN_TEST(test_parse_display_packet_mode_unknown_when_bogey_is_digit);
    RUN_TEST(test_mode_observation_survives_alert_glyph_until_session_reset);
    RUN_TEST(test_version_qualified_aux1_mode_is_authoritative_during_alerts);
    RUN_TEST(test_version_qualified_aux1_rejects_advanced_logic_in_euro_mode);
    RUN_TEST(test_aux1_mode_is_not_claimed_before_supported_firmware);
    RUN_TEST(test_parse_display_packet_softmuted_tracks_aux0_bit_0);
    RUN_TEST(test_parse_display_packet_softmuted_independent_of_led_mute);
    RUN_TEST(test_display_metadata_tracks_transport_and_version_qualified_bits);
    RUN_TEST(test_parse_display_packet_suppresses_bands_when_system_status_clear);
    RUN_TEST(test_parse_display_packet_rejects_short_payload);
    RUN_TEST(test_parse_display_packet_min_payload_clears_system_status_when_aux0_zero);
    RUN_TEST(test_parse_display_packet_reports_bands_when_system_status_set);
    RUN_TEST(test_display_packet_records_display_state_without_suppressing_alert_data);
    RUN_TEST(test_parse_resp_all_volume_populates_volume_fields);
    RUN_TEST(test_resp_all_volume_overrides_display_aux2_inference);
    RUN_TEST(test_parse_resp_all_volume_rejects_short_payload);
    RUN_TEST(test_parse_resp_all_volume_accepts_no_checksum_originator_width);
    RUN_TEST(test_canonical_resp_all_volume_updates_state);
    RUN_TEST(test_noncanonical_resp_all_volume_shapes_do_not_update_state);
    RUN_TEST(test_resp_all_volume_rejects_out_of_range_fields_atomically);
    RUN_TEST(test_canonical_width_wrong_id_does_not_update_volume);
    RUN_TEST(test_decode_signal_bars_renders_valid_bitmaps_literally_and_fails_loud);
    RUN_TEST(test_all_v1_bitmaps_are_bounded_by_eight_protocol_leds);
    RUN_TEST(test_bluetooth_indicator_requires_supported_version_and_accepts_both_blink_images);
    RUN_TEST(test_sweep_collectors_poison_conflicts_and_require_exact_max_set);
    RUN_TEST(test_sweep_responses_require_canonical_destination_checksum_and_index_bits);
    RUN_TEST(test_v1_flow_control_packets_require_their_specified_destinations);
    return UNITY_END();
}
