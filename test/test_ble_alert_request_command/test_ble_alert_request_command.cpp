#include <unity.h>

#include <cstdint>

#include "../mocks/Arduino.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#define private public
#include "../../src/ble_client.h"
#undef private
#include "../mocks/ble_client_callback_stubs.h"

V1BLEClient::V1BLEClient() {}
V1BLEClient::~V1BLEClient() {}
bool V1BLEClient::isConnected() {
    return connected_.load(std::memory_order_acquire) && pClient_ && pClient_->isConnected();
}

#include "../../src/ble_commands.cpp"

namespace {

struct AlertRequestHarness {
    V1BLEClient client;
    NimBLEClient link;
    NimBLERemoteCharacteristic command{"B6D4"};

    explicit AlertRequestHarness(uint32_t generation) {
        link.setConnected(true);
        client.connected_.store(true, std::memory_order_release);
        client.bleState_ = BLEState::CONNECTED;
        client.sessionGeneration_.store(generation, std::memory_order_release);
        client.pClient_ = &link;
        client.pCommandChar_ = &command;
        client.onV1DisplayFlowControl(false);
    }
};

} // namespace

void setUp() {}
void tearDown() {}

void test_command_guard_uses_successful_send_time_for_exact_boundary() {
    AlertRequestHarness harness(7);

    mockMillis = 5050;
    TEST_ASSERT_TRUE(harness.client.requestAlertData());
    TEST_ASSERT_EQUAL_UINT32(1, harness.command.writeValueCalls());

    // The stale owner runs 1001 ms after connection, but only 951 ms after
    // the delayed initial transmission. It must not reach the transport.
    mockMillis = 6001;
    TEST_ASSERT_TRUE(harness.client.requestAlertData());
    TEST_ASSERT_EQUAL_UINT32(1, harness.command.writeValueCalls());

    mockMillis = 6049;
    TEST_ASSERT_TRUE(harness.client.requestAlertData());
    TEST_ASSERT_EQUAL_UINT32(1, harness.command.writeValueCalls());

    mockMillis = 6050;
    TEST_ASSERT_TRUE(harness.client.requestAlertData());
    TEST_ASSERT_EQUAL_UINT32(2, harness.command.writeValueCalls());

    mockMillis = 7049;
    TEST_ASSERT_TRUE(harness.client.requestAlertData());
    TEST_ASSERT_EQUAL_UINT32(2, harness.command.writeValueCalls());

    mockMillis = 7050;
    TEST_ASSERT_TRUE(harness.client.requestAlertData());
    TEST_ASSERT_EQUAL_UINT32(3, harness.command.writeValueCalls());
}

void test_time_slice_holdoff_defers_v1_bound_commands_until_display_release() {
    AlertRequestHarness harness(6);
    harness.client.onV1DisplayFlowControl(true);

    mockMillis = 1000;
    TEST_ASSERT_FALSE(harness.client.requestCurrentVolume());
    TEST_ASSERT_EQUAL_UINT32(0, harness.command.writeValueCalls());

    harness.client.onV1DisplayFlowControl(false);
    TEST_ASSERT_TRUE(harness.client.requestCurrentVolume());
    TEST_ASSERT_EQUAL_UINT32(1, harness.command.writeValueCalls());
}

void test_failed_transport_write_does_not_consume_command_guard_slot() {
    AlertRequestHarness harness(7);
    harness.command.setWriteValueResult(false);
    TEST_ASSERT_TRUE(harness.client.needsAlertDataStartRecovery());

    mockMillis = 20000;
    TEST_ASSERT_FALSE(harness.client.requestAlertData());
    TEST_ASSERT_EQUAL_UINT32(1, harness.command.writeValueCalls());
    TEST_ASSERT_TRUE(harness.client.needsAlertDataStartRecovery());

    harness.command.setWriteValueResult(true);
    mockMillis = 20005;
    TEST_ASSERT_TRUE(harness.client.requestAlertData());
    TEST_ASSERT_EQUAL_UINT32(2, harness.command.writeValueCalls());
    TEST_ASSERT_FALSE(harness.client.needsAlertDataStartRecovery());

    mockMillis = 21004;
    TEST_ASSERT_TRUE(harness.client.requestAlertData());
    TEST_ASSERT_EQUAL_UINT32(2, harness.command.writeValueCalls());

    mockMillis = 21005;
    TEST_ASSERT_TRUE(harness.client.requestAlertData());
    TEST_ASSERT_EQUAL_UINT32(3, harness.command.writeValueCalls());
}

void test_new_session_generation_gets_an_immediate_first_command() {
    AlertRequestHarness harness(7);

    mockMillis = 30000;
    TEST_ASSERT_TRUE(harness.client.requestAlertData());
    TEST_ASSERT_EQUAL_UINT32(1, harness.command.writeValueCalls());
    TEST_ASSERT_FALSE(harness.client.needsAlertDataStartRecovery());

    harness.client.sessionGeneration_.store(9, std::memory_order_release);
    TEST_ASSERT_TRUE(harness.client.needsAlertDataStartRecovery());
    mockMillis = 30010;
    TEST_ASSERT_TRUE(harness.client.requestAlertData());
    TEST_ASSERT_EQUAL_UINT32(2, harness.command.writeValueCalls());
    TEST_ASSERT_FALSE(harness.client.needsAlertDataStartRecovery());

    mockMillis = 30020;
    TEST_ASSERT_TRUE(harness.client.requestAlertData());
    TEST_ASSERT_EQUAL_UINT32(2, harness.command.writeValueCalls());

    mockMillis = 31010;
    TEST_ASSERT_TRUE(harness.client.requestAlertData());
    TEST_ASSERT_EQUAL_UINT32(3, harness.command.writeValueCalls());
}

void test_alert_start_recovery_waits_for_subscription_and_optional_followups() {
    AlertRequestHarness harness(13);
    harness.client.bleState_ = BLEState::SUBSCRIBING;
    TEST_ASSERT_FALSE(harness.client.needsAlertDataStartRecovery());

    harness.client.bleState_ = BLEState::CONNECTED;
    harness.client.connectedFollowupStep_ = V1BLEClient::ConnectedFollowupStep::REQUEST_ALERT_DATA;
    TEST_ASSERT_FALSE(harness.client.needsAlertDataStartRecovery());
    harness.client.connectedFollowupStep_ = V1BLEClient::ConnectedFollowupStep::REQUEST_VERSION;
    TEST_ASSERT_FALSE(harness.client.needsAlertDataStartRecovery());
    harness.client.connectedFollowupStep_ = V1BLEClient::ConnectedFollowupStep::NONE;
    TEST_ASSERT_TRUE(harness.client.needsAlertDataStartRecovery());

    harness.client.bleState_ = BLEState::QUIESCING;
    TEST_ASSERT_FALSE(harness.client.needsAlertDataStartRecovery());
}

void test_command_guard_interval_survives_millis_wrap() {
    AlertRequestHarness harness(11);

    mockMillis = UINT32_MAX - 500;
    TEST_ASSERT_TRUE(harness.client.requestAlertData());
    TEST_ASSERT_EQUAL_UINT32(1, harness.command.writeValueCalls());

    mockMillis = 498;
    TEST_ASSERT_TRUE(harness.client.requestAlertData());
    TEST_ASSERT_EQUAL_UINT32(1, harness.command.writeValueCalls());

    mockMillis = 499;
    TEST_ASSERT_TRUE(harness.client.requestAlertData());
    TEST_ASSERT_EQUAL_UINT32(2, harness.command.writeValueCalls());
}

void test_settings_apply_commands_match_vendor_frames_exactly() {
    AlertRequestHarness harness(17);

    mockMillis = 100000;
    TEST_ASSERT_TRUE(harness.client.requestCurrentVolume());
    const uint8_t currentVolumeRequest[] = {0xAA, 0xDA, 0xE6, 0x37, 0x01, 0xA2, 0xAB};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(currentVolumeRequest, harness.command.lastWriteValue().data(),
                                  sizeof(currentVolumeRequest));

    mockMillis += 5;
    TEST_ASSERT_TRUE(harness.client.setDisplayOn(false));
    const uint8_t displayOff[] = {0xAA, 0xDA, 0xE6, 0x32, 0x01, 0x9D, 0xAB};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(displayOff, harness.command.lastWriteValue().data(), sizeof(displayOff));

    mockMillis += 5;
    TEST_ASSERT_TRUE(harness.client.setMode(2));
    const uint8_t modeLogic[] = {0xAA, 0xDA, 0xE6, 0x36, 0x02, 0x02, 0xA4, 0xAB};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(modeLogic, harness.command.lastWriteValue().data(), sizeof(modeLogic));

    mockMillis += 5;
    TEST_ASSERT_TRUE(harness.client.setVolume(7, 3));
    const uint8_t temporaryVolume[] = {0xAA, 0xDA, 0xE6, 0x39, 0x04, 0x07, 0x03, 0x00, 0xB1, 0xAB};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(temporaryVolume, harness.command.lastWriteValue().data(),
                                  sizeof(temporaryVolume));

    mockMillis += 5;
    const uint8_t effectiveUserBytes[] = {0xFE, 0xCF, 0x5F, 0xAA, 0xA4, 0x5A};
    TEST_ASSERT_TRUE(harness.client.writeUserBytesExact(effectiveUserBytes));
    const uint8_t userBytesWrite[] = {0xAA, 0xDA, 0xE6, 0x13, 0x07, 0xFE, 0xCF,
                                      0x5F, 0xAA, 0xA4, 0x5A, 0x58, 0xAB};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(userBytesWrite, harness.command.lastWriteValue().data(),
                                  sizeof(userBytesWrite));
}

void test_phase_four_commands_match_vendor_frames_and_firmware_gates() {
    AlertRequestHarness harness(19);
    mockMillis = 200000;
    harness.client.onV1FirmwareVersionReceived(41031);
    TEST_ASSERT_FALSE(harness.client.setDisplayOn(false, true));
    TEST_ASSERT_TRUE(harness.client.setDisplayOn(false, false));
    const uint8_t legacyDisplayOff[] = {0xAA, 0xDA, 0xE6, 0x32, 0x01, 0x9D, 0xAB};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(legacyDisplayOff, harness.command.lastWriteValue().data(),
                                  sizeof(legacyDisplayOff));

    mockMillis += 5;
    harness.client.onV1FirmwareVersionReceived(41032);
    TEST_ASSERT_TRUE(harness.client.setDisplayOn(false, true));
    const uint8_t displayOffKeepBt[] = {0xAA, 0xDA, 0xE6, 0x32, 0x02, 0x01, 0x9F, 0xAB};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(displayOffKeepBt, harness.command.lastWriteValue().data(),
                                  sizeof(displayOffKeepBt));

    mockMillis += 5;
    harness.client.onV1FirmwareVersionReceived(41037);
    TEST_ASSERT_EQUAL_INT(SendResult::SENT, harness.client.setVolumeResult(9, 0, 0x07));
    const uint8_t savedFeedbackVolume[] = {0xAA, 0xDA, 0xE6, 0x39, 0x04, 0x09, 0x00, 0x07, 0xB7, 0xAB};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(savedFeedbackVolume, harness.command.lastWriteValue().data(),
                                  sizeof(savedFeedbackVolume));
    TEST_ASSERT_EQUAL_INT(SendResult::FAILED, harness.client.setVolumeResult(10, 0, 0));
    TEST_ASSERT_EQUAL_INT(SendResult::FAILED, harness.client.setVolumeResult(1, 0, 0x08));

    mockMillis += 5;
    harness.client.onV1FirmwareVersionReceived(41038);
    TEST_ASSERT_EQUAL_INT(SendResult::SENT, harness.client.setVolumeResult(1, 0, 0x08));
    TEST_ASSERT_EQUAL_INT(SendResult::FAILED, harness.client.setVolumeResult(1, 0, 0x0C));
    mockMillis += 5;
    TEST_ASSERT_TRUE(harness.client.requestAllVolume());
    const uint8_t allVolume[] = {0xAA, 0xDA, 0xE6, 0x3C, 0x01, 0xA7, 0xAB};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(allVolume, harness.command.lastWriteValue().data(), sizeof(allVolume));

    mockMillis += 5;
    TEST_ASSERT_TRUE(harness.client.requestAllSweepDefinitions());
    const uint8_t allDefinitions[] = {0xAA, 0xDA, 0xE6, 0x16, 0x01, 0x81, 0xAB};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(allDefinitions, harness.command.lastWriteValue().data(),
                                  sizeof(allDefinitions));
    mockMillis += 5;
    TEST_ASSERT_EQUAL_INT(SendResult::SENT,
                          harness.client.writeSweepDefinition(0, 24050, 24150, false));
    const uint8_t firstDefinition[] = {0xAA, 0xDA, 0xE6, 0x15, 0x06, 0x80,
                                       0x5E, 0x56, 0x5D, 0xF2, 0x08, 0xAB};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(firstDefinition, harness.command.lastWriteValue().data(),
                                  sizeof(firstDefinition));
    mockMillis += 5;
    TEST_ASSERT_EQUAL_INT(SendResult::FAILED,
                          harness.client.writeSweepDefinition(1, 0, 0, true));
    mockMillis += 5;
    TEST_ASSERT_EQUAL_INT(SendResult::SENT,
                          harness.client.writeSweepDefinition(0, 24050, 24150, true));
    const uint8_t finalUsedDefinition[] = {0xAA, 0xDA, 0xE6, 0x15, 0x06, 0xC0,
                                          0x5E, 0x56, 0x5D, 0xF2, 0x48, 0xAB};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(finalUsedDefinition, harness.command.lastWriteValue().data(),
                                  sizeof(finalUsedDefinition));

    mockMillis += 5;
    TEST_ASSERT_EQUAL_INT(SendResult::SENT, harness.client.factoryResetDetector());
    const uint8_t factory[] = {0xAA, 0xDA, 0xE6, 0x14, 0x01, 0x7F, 0xAB};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(factory, harness.command.lastWriteValue().data(), sizeof(factory));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_command_guard_uses_successful_send_time_for_exact_boundary);
    RUN_TEST(test_time_slice_holdoff_defers_v1_bound_commands_until_display_release);
    RUN_TEST(test_failed_transport_write_does_not_consume_command_guard_slot);
    RUN_TEST(test_new_session_generation_gets_an_immediate_first_command);
    RUN_TEST(test_alert_start_recovery_waits_for_subscription_and_optional_followups);
    RUN_TEST(test_command_guard_interval_survives_millis_wrap);
    RUN_TEST(test_settings_apply_commands_match_vendor_frames_exactly);
    RUN_TEST(test_phase_four_commands_match_vendor_frames_and_firmware_gates);
    return UNITY_END();
}
