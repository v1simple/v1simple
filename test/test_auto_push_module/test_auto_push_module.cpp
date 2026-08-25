#include <unity.h>

#include "../mocks/Arduino.h"
#include "../mocks/ble_client.h"
#include "../mocks/display.h"
#include "../mocks/packet_parser.h"
#include "../mocks/settings.h"
#include "../mocks/v1_profiles.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#include "../../src/modules/quiet/quiet_coordinator_module.cpp"
#include "../../src/modules/auto_push/auto_push_module.cpp"

static V1BLEClient ble;
static V1Display display;
static PacketParser parser;
static SettingsManager settings;
static V1ProfileManager profiles;
static QuietCoordinatorModule quiet;
static AutoPushModule module;

static void at(unsigned long now) {
    mockMillis = now;
    module.process();
}

static bool statusContains(const char* text) {
    return module.getStatusJson().indexOf(text) >= 0;
}

static void configureProfileSlot(bool configureVolumes = false) {
    settings.slotConfigs[0].profileName = "ROAD";
    settings.slotConfigs[0].mode = V1_MODE_LOGIC;
    settings.slotVolumes[0] = configureVolumes ? 6 : 0xFF;
    settings.slotMuteVolumes[0] = configureVolumes ? 2 : 0xFF;
    profiles.loadProfileResult = true;
    profiles.loadableProfileName = "ROAD";
    profiles.loadableProfile.name = "ROAD";
    const uint8_t bytes[6] = {0x01, 0x22, 0x33, 0x44, 0x55, 0x66};
    std::memcpy(profiles.loadableProfile.settings.bytes, bytes, sizeof(bytes));
}

static void queueAndReachFirstVerification() {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AutoPushModule::QueueResult::QUEUED),
                          static_cast<int>(module.queueSlotPush(0)));
    TEST_ASSERT_TRUE(statusContains("\"result\":\"queued\""));
    at(100); // WaitReady -> Profile
    at(100); // write
    at(130); // request readback -> verify wait
    TEST_ASSERT_TRUE(statusContains("\"step\":\"ProfileVerify\""));
}

static void finishIndependentSettings(unsigned long start) {
    at(start);      // Display
    at(start + 30); // Mode
    at(start + 60); // Volume / terminal
}

void setUp() {
    ble.reset();
    display = V1Display{};
    parser.reset();
    settings = SettingsManager{};
    profiles.reset();
    mockMillis = 0;
    mockMicros = 0;
    quiet.begin(&ble, &parser);
    module = AutoPushModule{};
    module.begin(&settings, &profiles, &ble, &display, &quiet);
}

void tearDown() {}

void test_matching_readback_is_required_before_success() {
    configureProfileSlot(true);
    queueAndReachFirstVerification();

    TEST_ASSERT_FALSE(statusContains("\"result\":\"succeeded\""));
    ble.setUserBytesVerificationStatus(V1BLEClient::UserBytesVerificationStatus::MATCH);
    at(130);
    finishIndependentSettings(160);

    TEST_ASSERT_FALSE(module.isActive());
    TEST_ASSERT_TRUE(statusContains("\"result\":\"succeeded\""));
    TEST_ASSERT_TRUE(statusContains("\"profile\":{\"requested\":true,\"applied\":true}"));
    TEST_ASSERT_TRUE(statusContains("\"volume\":{\"requested\":true,\"applied\":true}"));
    TEST_ASSERT_EQUAL_UINT8(6, ble.lastVolume);
    TEST_ASSERT_EQUAL_UINT8(2, ble.lastMuteVolume);
}

void test_mismatch_retries_then_reports_partial_after_independent_settings() {
    configureProfileSlot();
    queueAndReachFirstVerification();

    unsigned long now = 130;
    for (int attempt = 0; attempt <= 5; ++attempt) {
        ble.setUserBytesVerificationStatus(V1BLEClient::UserBytesVerificationStatus::MISMATCH);
        at(now);
        if (attempt < 5) {
            now += 30;
            at(now); // rewrite
            now += 30;
            at(now); // rerequest
        }
    }
    finishIndependentSettings(now + 30);

    TEST_ASSERT_TRUE(statusContains("\"result\":\"partial\""));
    TEST_ASSERT_TRUE(statusContains("\"reason\":\"profile_verify_mismatch\""));
    TEST_ASSERT_EQUAL_INT(6, ble.writeUserBytesCalls);
    TEST_ASSERT_EQUAL_INT(6, ble.requestUserBytesCalls);
    TEST_ASSERT_EQUAL_INT(1, display.drawProfileIndicatorCalls);
}

void test_mismatch_retry_can_recover_to_verified_success() {
    configureProfileSlot();
    queueAndReachFirstVerification();

    ble.setUserBytesVerificationStatus(V1BLEClient::UserBytesVerificationStatus::MISMATCH);
    at(130);
    at(160); // rewrite
    at(190); // rerequest
    ble.setUserBytesVerificationStatus(V1BLEClient::UserBytesVerificationStatus::MATCH);
    at(190);
    finishIndependentSettings(220);

    TEST_ASSERT_TRUE(statusContains("\"result\":\"succeeded\""));
    TEST_ASSERT_TRUE(statusContains("\"reason\":\"none\""));
    TEST_ASSERT_EQUAL_INT(2, ble.writeUserBytesCalls);
}

void test_read_request_failure_retries_then_reports_partial() {
    configureProfileSlot();
    ble.requestUserBytesResult = false;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AutoPushModule::QueueResult::QUEUED),
                          static_cast<int>(module.queueSlotPush(0)));
    at(100);
    at(100);

    unsigned long now = 130;
    for (int attempt = 0; attempt <= 5; ++attempt) {
        at(now); // failed request
        if (attempt < 5) {
            now += 30;
            at(now); // rewrite
            now += 30;
        }
    }
    finishIndependentSettings(now + 30);

    TEST_ASSERT_TRUE(statusContains("\"result\":\"partial\""));
    TEST_ASSERT_TRUE(statusContains("\"reason\":\"profile_read_request_failed\""));
}

void test_verification_timeout_retries_then_reports_partial() {
    configureProfileSlot();
    queueAndReachFirstVerification();

    unsigned long now = 130;
    for (int attempt = 0; attempt <= 5; ++attempt) {
        now += 1500;
        at(now); // timeout
        if (attempt < 5) {
            now += 30;
            at(now); // rewrite
            now += 30;
            at(now); // rerequest
        }
    }
    finishIndependentSettings(now + 30);

    TEST_ASSERT_TRUE(statusContains("\"result\":\"partial\""));
    TEST_ASSERT_TRUE(statusContains("\"reason\":\"profile_verify_timeout\""));
}

void test_disconnect_is_terminal_and_never_reports_success() {
    configureProfileSlot();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AutoPushModule::QueueResult::QUEUED),
                          static_cast<int>(module.queueSlotPush(0)));
    ble.setConnected(false);
    at(100);

    TEST_ASSERT_FALSE(module.isActive());
    TEST_ASSERT_TRUE(statusContains("\"result\":\"failed\""));
    TEST_ASSERT_TRUE(statusContains("\"reason\":\"disconnected\""));
}

void test_incomplete_volume_pair_is_rejected_before_queueing() {
    configureProfileSlot();
    settings.slotVolumes[0] = 7;
    settings.slotMuteVolumes[0] = 0xFF;

    TEST_ASSERT_EQUAL_INT(static_cast<int>(AutoPushModule::QueueResult::INVALID_VOLUME_PAIR),
                          static_cast<int>(module.queueSlotPush(0)));
    TEST_ASSERT_FALSE(module.isActive());
    TEST_ASSERT_EQUAL_INT(0, ble.writeUserBytesCalls);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_matching_readback_is_required_before_success);
    RUN_TEST(test_mismatch_retries_then_reports_partial_after_independent_settings);
    RUN_TEST(test_mismatch_retry_can_recover_to_verified_success);
    RUN_TEST(test_read_request_failure_retries_then_reports_partial);
    RUN_TEST(test_verification_timeout_retries_then_reports_partial);
    RUN_TEST(test_disconnect_is_terminal_and_never_reports_success);
    RUN_TEST(test_incomplete_volume_pair_is_rejected_before_queueing);
    return UNITY_END();
}
