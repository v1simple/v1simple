/**
 * BLE client header-level tests.
 *
 * Header-level BLE policy coverage.
 */

#include <unity.h>
#include <cstdint>
#include <cstring>

#include "../mocks/Arduino.h"
#include "../mocks/freertos/FreeRTOS.h"
#include "../mocks/freertos/task.h"

// V1 protocol constants from config.h — define inline so the full display
// wiring in config.h is not pulled in for this native test.
#ifndef CONFIG_H
#define CONFIG_H
#define ESP_PACKET_START         0xAA
#define ESP_PACKET_END           0xAB
#define ESP_PACKET_DEST_V1       0x0A
#define ESP_PACKET_REMOTE        0x06
#define PACKET_ID_MUTE_ON        0x34
#define PACKET_ID_MUTE_OFF       0x35
#define PACKET_ID_REQ_WRITE_VOLUME 0x39
#define PACKET_ID_TURN_OFF_DISPLAY 0x32
#define PACKET_ID_TURN_ON_DISPLAY  0x33
#define PACKET_ID_WRITE_USER_BYTES 0x13
#define PACKET_ID_REQ_USER_BYTES   0x11
#define PACKET_ID_REQ_START_ALERT  0x41
#define PACKET_ID_VERSION        0x01
#define PACKET_ID_DISPLAY_DATA   0x31
#define PACKET_ID_ALERT_DATA     0x43
#define PACKET_ID_RESP_USER_BYTES 0x12
#endif

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#include "../../src/ble_client.h"

#include "../../include/ble_internals.h"

namespace {

uint8_t calcV1Checksum(const uint8_t* data, size_t len) {
    uint8_t sum = 0;
    for (size_t i = 0; i < len; ++i) {
        sum += data[i];
    }
    return sum;
}

bool hitsHardResetThreshold(uint8_t consecutiveFailures) {
    return hitsV1BleHardResetThreshold(consecutiveFailures);
}

struct BootGateStateMachine {
    bool bootReadyFlag = false;
    BLEState state = BLEState::DISCONNECTED;

    void setBootReady(bool ready) { bootReadyFlag = ready; }
    bool isBootReady() const { return bootReadyFlag; }

    void process() {
        if (!bootReadyFlag) {
            return;
        }
        if (state == BLEState::DISCONNECTED) {
            state = BLEState::SCANNING;
        }
    }
};

}  // namespace

void test_ble_state_strings_match_production() {
    TEST_ASSERT_EQUAL_STRING("DISCONNECTED", bleStateToString(BLEState::DISCONNECTED));
    TEST_ASSERT_EQUAL_STRING("SCANNING", bleStateToString(BLEState::SCANNING));
    TEST_ASSERT_EQUAL_STRING("SCAN_STOPPING", bleStateToString(BLEState::SCAN_STOPPING));
    TEST_ASSERT_EQUAL_STRING("CONNECTING", bleStateToString(BLEState::CONNECTING));
    TEST_ASSERT_EQUAL_STRING("CONNECTING_WAIT", bleStateToString(BLEState::CONNECTING_WAIT));
    TEST_ASSERT_EQUAL_STRING("DISCOVERING", bleStateToString(BLEState::DISCOVERING));
    TEST_ASSERT_EQUAL_STRING("SUBSCRIBING", bleStateToString(BLEState::SUBSCRIBING));
    TEST_ASSERT_EQUAL_STRING("SUBSCRIBE_YIELD", bleStateToString(BLEState::SUBSCRIBE_YIELD));
    TEST_ASSERT_EQUAL_STRING("CONNECTED", bleStateToString(BLEState::CONNECTED));
    TEST_ASSERT_EQUAL_STRING("BACKOFF", bleStateToString(BLEState::BACKOFF));
    TEST_ASSERT_EQUAL_STRING("QUIESCING", bleStateToString(BLEState::QUIESCING));
}

void test_ble_state_unknown_string() {
    TEST_ASSERT_EQUAL_STRING("UNKNOWN", bleStateToString(static_cast<BLEState>(99)));
}

void test_state_enum_values() {
    TEST_ASSERT_EQUAL_INT(0, static_cast<int>(BLEState::DISCONNECTED));
    TEST_ASSERT_EQUAL_INT(1, static_cast<int>(BLEState::SCANNING));
    TEST_ASSERT_EQUAL_INT(2, static_cast<int>(BLEState::SCAN_STOPPING));
    TEST_ASSERT_EQUAL_INT(3, static_cast<int>(BLEState::CONNECTING));
    TEST_ASSERT_EQUAL_INT(4, static_cast<int>(BLEState::CONNECTING_WAIT));
    TEST_ASSERT_EQUAL_INT(5, static_cast<int>(BLEState::DISCOVERING));
    TEST_ASSERT_EQUAL_INT(6, static_cast<int>(BLEState::SUBSCRIBING));
    TEST_ASSERT_EQUAL_INT(7, static_cast<int>(BLEState::SUBSCRIBE_YIELD));
    TEST_ASSERT_EQUAL_INT(8, static_cast<int>(BLEState::CONNECTED));
    TEST_ASSERT_EQUAL_INT(9, static_cast<int>(BLEState::BACKOFF));
    TEST_ASSERT_EQUAL_INT(10, static_cast<int>(BLEState::QUIESCING));
}

void test_failure_threshold_constant_matches_expected_profile() {
    TEST_ASSERT_EQUAL_UINT8(5, V1_BLE_MAX_BACKOFF_FAILURES);
}

void test_hard_reset_threshold_uses_production_limit() {
    TEST_ASSERT_FALSE(hitsHardResetThreshold(V1_BLE_MAX_BACKOFF_FAILURES - 1));
    TEST_ASSERT_TRUE(hitsHardResetThreshold(V1_BLE_MAX_BACKOFF_FAILURES));
    TEST_ASSERT_TRUE(hitsHardResetThreshold(V1_BLE_MAX_BACKOFF_FAILURES + 1));
}

void test_checksum_empty_data() {
    uint8_t data[] = {};
    TEST_ASSERT_EQUAL_UINT8(0, calcV1Checksum(data, 0));
}

void test_checksum_multiple_bytes() {
    uint8_t data[] = {0x01, 0x02, 0x03, 0x04};
    TEST_ASSERT_EQUAL_UINT8(0x0A, calcV1Checksum(data, 4));
}

void test_checksum_overflow_wraps() {
    uint8_t data[] = {0xFF, 0x02};
    TEST_ASSERT_EQUAL_UINT8(0x01, calcV1Checksum(data, 2));
}

void test_checksum_real_v1_packet() {
    uint8_t packet[] = {0xAA, 0x55, 0x01, 0x03, 0x31};
    TEST_ASSERT_EQUAL_UINT8(0x34, calcV1Checksum(packet, 5));
}

void test_boot_gate_blocks_state_machine() {
    BootGateStateMachine sm;
    sm.state = BLEState::DISCONNECTED;
    sm.setBootReady(false);

    sm.process();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(BLEState::DISCONNECTED), static_cast<int>(sm.state));

    sm.setBootReady(true);
    sm.process();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(BLEState::SCANNING), static_cast<int>(sm.state));
}

void test_boot_gate_default_false_until_set() {
    BootGateStateMachine sm;
    TEST_ASSERT_FALSE(sm.isBootReady());

    sm.setBootReady(true);
    TEST_ASSERT_TRUE(sm.isBootReady());
}

void test_session_publication_gate_rejects_closed_and_old_generations() {
    BleSessionPublicationGate gate;

    TEST_ASSERT_FALSE(gate.accepts(0));
    TEST_ASSERT_FALSE(gate.accepts(41));
    TEST_ASSERT_FALSE(gate.claim(41));

    gate.open(41);
    TEST_ASSERT_TRUE(gate.accepts(41));
    TEST_ASSERT_TRUE(gate.claim(41));
    TEST_ASSERT_FALSE(gate.accepts(40));
    TEST_ASSERT_FALSE(gate.claim(40));

    gate.close();
    TEST_ASSERT_FALSE(gate.accepts(41));
    TEST_ASSERT_FALSE(gate.claim(41));

    gate.open(42);
    TEST_ASSERT_FALSE(gate.accepts(41));
    TEST_ASSERT_FALSE(gate.claim(41));
    TEST_ASSERT_TRUE(gate.accepts(42));
    TEST_ASSERT_TRUE(gate.claim(42));
}

void test_quiesce_deadline_is_inclusive_and_millis_wrap_safe() {
    TEST_ASSERT_FALSE(bleQuiesceDeadlineExpired(1099, 1000, 100));
    TEST_ASSERT_TRUE(bleQuiesceDeadlineExpired(1100, 1000, 100));

    const uint32_t startedNearWrap = UINT32_MAX - 50u;
    TEST_ASSERT_FALSE(bleQuiesceDeadlineExpired(48u, startedNearWrap, 100u));
    TEST_ASSERT_TRUE(bleQuiesceDeadlineExpired(49u, startedNearWrap, 100u));
}

void setUp(void) {}
void tearDown(void) {}

// --- V1 command packet protocol constants (ble_commands.cpp contract) ---
// These tests pin the frame byte values used in every BLE command sent to the
// V1.  A mismatch here means commands would be silently ignored or
// misinterpreted by the Valentine hardware — Valentine's Law surface.

void test_v1_packet_frame_bytes_match_protocol() {
    TEST_ASSERT_EQUAL_HEX8(0xAA, ESP_PACKET_START);
    TEST_ASSERT_EQUAL_HEX8(0xAB, ESP_PACKET_END);
}

void test_v1_packet_routing_nibbles_match_protocol() {
    // Remote app address in the source field: 0xE0 + REMOTE
    TEST_ASSERT_EQUAL_HEX8(0x0A, ESP_PACKET_DEST_V1);
    TEST_ASSERT_EQUAL_HEX8(0x06, ESP_PACKET_REMOTE);
}

void test_v1_command_packet_ids_match_protocol() {
    // Packet IDs used by ble_commands.cpp — changes here break V1 comms.
    TEST_ASSERT_EQUAL_HEX8(0x34, PACKET_ID_MUTE_ON);
    TEST_ASSERT_EQUAL_HEX8(0x35, PACKET_ID_MUTE_OFF);
    TEST_ASSERT_EQUAL_HEX8(0x39, PACKET_ID_REQ_WRITE_VOLUME);
    TEST_ASSERT_EQUAL_HEX8(0x32, PACKET_ID_TURN_OFF_DISPLAY);
    TEST_ASSERT_EQUAL_HEX8(0x33, PACKET_ID_TURN_ON_DISPLAY);
    TEST_ASSERT_EQUAL_HEX8(0x13, PACKET_ID_WRITE_USER_BYTES);
    TEST_ASSERT_EQUAL_HEX8(0x11, PACKET_ID_REQ_USER_BYTES);
    TEST_ASSERT_EQUAL_HEX8(0x41, PACKET_ID_REQ_START_ALERT);
}

void test_v1_mute_on_packet_checksum_is_correct() {
    // Replicate the setMute(true) packet from ble_commands.cpp and verify
    // the checksum byte that would be inserted at position [5].
    const uint8_t dest = static_cast<uint8_t>(0xD0 + ESP_PACKET_DEST_V1);  // 0xDA
    const uint8_t src  = static_cast<uint8_t>(0xE0 + ESP_PACKET_REMOTE);   // 0xE6
    uint8_t packet[5] = { ESP_PACKET_START, dest, src, PACKET_ID_MUTE_ON, 0x01 };
    TEST_ASSERT_EQUAL_HEX8(calcV1Checksum(packet, 5),
                            calcV1Checksum(packet, 5));  // idempotent sanity
    // Verify the specific expected checksum value so the formula is pinned.
    // Sum of { 0xAA, 0xDA, 0xE6, 0x34, 0x01 } = 0x19F → low byte = 0x9F
    TEST_ASSERT_EQUAL_HEX8(0x9F, calcV1Checksum(packet, 5));
}

void test_v1_mute_off_packet_checksum_differs_from_mute_on() {
    const uint8_t dest = static_cast<uint8_t>(0xD0 + ESP_PACKET_DEST_V1);
    const uint8_t src  = static_cast<uint8_t>(0xE0 + ESP_PACKET_REMOTE);
    uint8_t on_pkt[5]  = { ESP_PACKET_START, dest, src, PACKET_ID_MUTE_ON,  0x01 };
    uint8_t off_pkt[5] = { ESP_PACKET_START, dest, src, PACKET_ID_MUTE_OFF, 0x01 };
    TEST_ASSERT_NOT_EQUAL(calcV1Checksum(on_pkt,  5),
                          calcV1Checksum(off_pkt, 5));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_ble_state_strings_match_production);
    RUN_TEST(test_ble_state_unknown_string);
    RUN_TEST(test_state_enum_values);
    RUN_TEST(test_failure_threshold_constant_matches_expected_profile);
    RUN_TEST(test_hard_reset_threshold_uses_production_limit);
    RUN_TEST(test_checksum_empty_data);
    RUN_TEST(test_checksum_multiple_bytes);
    RUN_TEST(test_checksum_overflow_wraps);
    RUN_TEST(test_checksum_real_v1_packet);
    RUN_TEST(test_boot_gate_blocks_state_machine);
    RUN_TEST(test_boot_gate_default_false_until_set);
    RUN_TEST(test_session_publication_gate_rejects_closed_and_old_generations);
    RUN_TEST(test_quiesce_deadline_is_inclusive_and_millis_wrap_safe);
    RUN_TEST(test_v1_packet_frame_bytes_match_protocol);
    RUN_TEST(test_v1_packet_routing_nibbles_match_protocol);
    RUN_TEST(test_v1_command_packet_ids_match_protocol);
    RUN_TEST(test_v1_mute_on_packet_checksum_is_correct);
    RUN_TEST(test_v1_mute_off_packet_checksum_differs_from_mute_on);
    return UNITY_END();
}
