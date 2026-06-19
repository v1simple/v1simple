#include <unity.h>

#include "../mocks/Arduino.h"
#include "../mocks/settings.h"
#include "../mocks/v1_profiles.h"
#include "../mocks/ble_client.h"
#include "../mocks/display.h"
#include "../mocks/packet_parser.h"

#include <cstring>

#include "../../src/perf_metrics.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

PerfCounters perfCounters;
PerfExtendedMetrics perfExtended;
void perfRecordDisplayRenderUs(uint32_t /*us*/) {}
void perfRecordDisplayScenarioRenderUs(uint32_t /*us*/) {}

#include "../../src/modules/quiet/quiet_coordinator_module.cpp"
#include "../../src/modules/auto_push/auto_push_module.cpp"

SettingsManager settingsManager;
static V1ProfileManager profileManager;
static V1BLEClient bleClient;
static V1Display display;
static PacketParser parser;
static QuietCoordinatorModule quiet;
static AutoPushModule module;

static V1Profile makeProfile(const char* name, uint8_t byte0 = 0xFF) {
    V1Profile profile;
    profile.name = name;
    profile.settings.bytes[0] = byte0;
    return profile;
}

static void advanceTime(unsigned long deltaMs) {
    mockMillis += deltaMs;
    mockMicros = mockMillis * 1000;
}

static void runUntilIdle(size_t maxTicks = 32) {
    for (size_t tick = 0; tick < maxTicks; ++tick) {
        module.process();
        if (!module.isActive()) {
            return;
        }
        advanceTime(40);
    }

    TEST_FAIL_MESSAGE("AutoPushModule did not become idle");
}

void setUp() {
    mockMillis = 1000;
    mockMicros = 1000000;
    perfCounters.reset();
    perfExtended.reset();

    settingsManager = SettingsManager{};
    profileManager.reset();
    bleClient.reset();
    display.reset();
    parser.reset();
    quiet.begin(&bleClient, &parser);

    module = AutoPushModule{};
    module.begin(&settingsManager, &profileManager, &bleClient, &display, &quiet);
}

void tearDown() {}

void test_start_runs_existing_auto_push_path() {
    bleClient.setConnected(true);
    settingsManager.setSlot(1, "Road", V1_MODE_LOGIC);
    settingsManager.setSlotVolumes(1, 6, 2);
    settingsManager.setSlotDarkMode(1, false);
    settingsManager.setSlotMuteToZero(1, true);

    profileManager.loadProfileResult = true;
    profileManager.loadableProfileName = "Road";
    profileManager.loadableProfile = makeProfile("Road", 0xFF);

    module.queueSlotPush(1);

    TEST_ASSERT_TRUE(module.isActive());
    TEST_ASSERT_EQUAL_UINT32(1, perfCounters.autoPushStarts.load());
    TEST_ASSERT_EQUAL_INT(1, display.drawProfileIndicatorCalls);
    TEST_ASSERT_EQUAL_INT(1, display.lastProfileIndicatorSlot);

    runUntilIdle();

    TEST_ASSERT_FALSE(module.isActive());
    TEST_ASSERT_EQUAL_INT(1, profileManager.loadProfileCalls);
    TEST_ASSERT_EQUAL_STRING("Road", profileManager.lastLoadProfileName.c_str());
    TEST_ASSERT_EQUAL_INT(1, bleClient.writeUserBytesCalls);
    TEST_ASSERT_EQUAL_HEX8(0xEF, bleClient.lastUserBytes[0]);
    TEST_ASSERT_EQUAL_INT(1, bleClient.startUserBytesVerificationCalls);
    TEST_ASSERT_EQUAL_INT(1, bleClient.requestUserBytesCalls);
    TEST_ASSERT_EQUAL_INT(1, bleClient.setDisplayOnCalls);
    TEST_ASSERT_TRUE(bleClient.lastDisplayOnValue);
    TEST_ASSERT_EQUAL_INT(1, bleClient.setModeCalls);
    TEST_ASSERT_EQUAL_UINT8(V1_MODE_LOGIC, bleClient.lastModeValue);
    TEST_ASSERT_EQUAL_INT(1, bleClient.setVolumeCalls);
    TEST_ASSERT_EQUAL_UINT8(6, bleClient.lastVolume);
    TEST_ASSERT_EQUAL_UINT8(2, bleClient.lastMuteVolume);
    TEST_ASSERT_EQUAL_UINT32(1, perfCounters.autoPushCompletes.load());
    TEST_ASSERT_EQUAL_UINT32(0, perfCounters.pushNowRetries.load());
    TEST_ASSERT_EQUAL_UINT32(0, perfCounters.pushNowFailures.load());
}

void test_queue_push_now_with_profile_override_skips_slot_mode_without_override() {
    bleClient.setConnected(true);
    settingsManager.setSlot(2, "Highway", V1_MODE_ADVANCED_LOGIC);
    settingsManager.setSlotDarkMode(2, true);
    settingsManager.setActiveSlot(0);

    profileManager.loadProfileResult = true;
    profileManager.loadableProfileName = "Quiet";
    profileManager.loadableProfile = makeProfile("Quiet", 0xA5);

    AutoPushModule::PushNowRequest request;
    request.slotIndex = 2;
    request.activateSlot = true;
    request.hasProfileOverride = true;
    request.profileName = "Quiet";

    const auto result = module.queuePushNow(request);

    TEST_ASSERT_EQUAL_INT(static_cast<int>(AutoPushModule::QueueResult::QUEUED),
                          static_cast<int>(result));
    TEST_ASSERT_TRUE(module.isActive());
    TEST_ASSERT_EQUAL_INT(2, settingsManager.get().activeSlot);
    TEST_ASSERT_EQUAL_INT(1, profileManager.loadProfileCalls);
    TEST_ASSERT_EQUAL_STRING("Quiet", profileManager.lastLoadProfileName.c_str());
    TEST_ASSERT_EQUAL_INT(1, display.drawProfileIndicatorCalls);
    TEST_ASSERT_EQUAL_INT(2, display.lastProfileIndicatorSlot);
    TEST_ASSERT_EQUAL_UINT32(0, perfCounters.autoPushStarts.load());

    runUntilIdle();

    TEST_ASSERT_FALSE(module.isActive());
    TEST_ASSERT_EQUAL_INT(0, bleClient.setModeCalls);
    TEST_ASSERT_EQUAL_INT(1, bleClient.setDisplayOnCalls);
    TEST_ASSERT_FALSE(bleClient.lastDisplayOnValue);
    TEST_ASSERT_EQUAL_INT(1, bleClient.writeUserBytesCalls);
    TEST_ASSERT_EQUAL_UINT32(0, perfCounters.autoPushCompletes.load());
    TEST_ASSERT_EQUAL_UINT32(0, perfCounters.pushNowRetries.load());
    TEST_ASSERT_EQUAL_UINT32(0, perfCounters.pushNowFailures.load());
}

void test_queue_push_now_retries_display_step_and_tracks_push_now_metrics() {
    bleClient.setConnected(true);
    bleClient.setDisplayOnFailuresRemaining = 2;

    settingsManager.setSlot(0, "Road", V1_MODE_UNKNOWN);
    settingsManager.setSlotDarkMode(0, true);

    profileManager.loadProfileResult = true;
    profileManager.loadableProfileName = "Road";
    profileManager.loadableProfile = makeProfile("Road", 0xFF);

    AutoPushModule::PushNowRequest request;
    request.slotIndex = 0;
    request.activateSlot = true;

    const auto result = module.queuePushNow(request);

    TEST_ASSERT_EQUAL_INT(static_cast<int>(AutoPushModule::QueueResult::QUEUED),
                          static_cast<int>(result));

    runUntilIdle();

    TEST_ASSERT_FALSE(module.isActive());
    TEST_ASSERT_EQUAL_INT(3, bleClient.setDisplayOnCalls);
    TEST_ASSERT_FALSE(bleClient.lastDisplayOnValue);
    TEST_ASSERT_EQUAL_UINT32(2, perfCounters.pushNowRetries.load());
    TEST_ASSERT_EQUAL_UINT32(0, perfCounters.pushNowFailures.load());
    TEST_ASSERT_EQUAL_UINT32(0, perfCounters.autoPushBusyRetries.load());
    TEST_ASSERT_EQUAL_UINT32(0, perfCounters.autoPushCompletes.load());
}

void test_start_while_active_does_not_override_inflight_request() {
    bleClient.setConnected(true);
    settingsManager.setSlot(1, "Road", V1_MODE_LOGIC);
    settingsManager.setSlotVolumes(1, 6, 2);
    settingsManager.setSlotDarkMode(1, false);
    settingsManager.setSlotMuteToZero(1, true);

    settingsManager.setSlot(2, "Road", V1_MODE_ADVANCED_LOGIC);
    settingsManager.setSlotVolumes(2, 3, 1);
    settingsManager.setSlotDarkMode(2, true);
    settingsManager.setSlotMuteToZero(2, false);

    profileManager.loadProfileResult = true;
    profileManager.loadableProfileName = "Road";
    profileManager.loadableProfile = makeProfile("Road", 0xFF);

    module.queueSlotPush(1);
    module.queueSlotPush(2);

    TEST_ASSERT_TRUE(module.isActive());
    TEST_ASSERT_EQUAL_UINT32(1, perfCounters.autoPushStarts.load());
    TEST_ASSERT_EQUAL_INT(1, display.drawProfileIndicatorCalls);
    TEST_ASSERT_EQUAL_INT(1, display.lastProfileIndicatorSlot);

    runUntilIdle();

    TEST_ASSERT_FALSE(module.isActive());
    TEST_ASSERT_EQUAL_STRING("Road", profileManager.lastLoadProfileName.c_str());
    TEST_ASSERT_EQUAL_INT(1, bleClient.setDisplayOnCalls);
    TEST_ASSERT_TRUE(bleClient.lastDisplayOnValue);
    TEST_ASSERT_EQUAL_INT(1, bleClient.setModeCalls);
    TEST_ASSERT_EQUAL_UINT8(V1_MODE_LOGIC, bleClient.lastModeValue);
    TEST_ASSERT_EQUAL_INT(1, bleClient.setVolumeCalls);
    TEST_ASSERT_EQUAL_UINT8(6, bleClient.lastVolume);
    TEST_ASSERT_EQUAL_UINT8(2, bleClient.lastMuteVolume);
}

void test_queue_slot_push_can_suppress_immediate_profile_indicator_draw() {
    bleClient.setConnected(true);
    settingsManager.setSlot(1, "Road", V1_MODE_LOGIC);
    settingsManager.setSlotVolumes(1, 6, 2);
    settingsManager.setSlotDarkMode(1, false);
    settingsManager.setSlotMuteToZero(1, true);

    profileManager.loadProfileResult = true;
    profileManager.loadableProfileName = "Road";
    profileManager.loadableProfile = makeProfile("Road", 0xFF);

    const auto result = module.queueSlotPush(1, false, false);

    TEST_ASSERT_EQUAL_INT(static_cast<int>(AutoPushModule::QueueResult::QUEUED),
                          static_cast<int>(result));
    TEST_ASSERT_TRUE(module.isActive());
    TEST_ASSERT_EQUAL_INT(0, display.drawProfileIndicatorCalls);

    runUntilIdle();

    TEST_ASSERT_FALSE(module.isActive());
    TEST_ASSERT_EQUAL_INT(1, bleClient.writeUserBytesCalls);
    TEST_ASSERT_EQUAL_UINT32(1, perfCounters.autoPushCompletes.load());
}

void test_alp_policy_disables_v1_laser_bit_on_profile_push() {
    bleClient.setConnected(true);
    settingsManager.settings.alpEnabled = true;
    settingsManager.settings.alpDisableV1LaserOnPush = true;
    settingsManager.setSlot(0, "Road", V1_MODE_UNKNOWN);

    profileManager.loadProfileResult = true;
    profileManager.loadableProfileName = "Road";
    profileManager.loadableProfile = makeProfile("Road", 0xFF);

    const auto result = module.queueSlotPush(0);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AutoPushModule::QueueResult::QUEUED),
                          static_cast<int>(result));

    runUntilIdle();

    TEST_ASSERT_EQUAL_INT(1, bleClient.writeUserBytesCalls);
    TEST_ASSERT_EQUAL_HEX8(0xF7, bleClient.lastUserBytes[0]);
    TEST_ASSERT_EQUAL_HEX8(0xF7, bleClient.lastVerifiedUserBytes[0]);
}

void test_alp_policy_toggle_can_keep_v1_laser_bit_on_profile_push() {
    bleClient.setConnected(true);
    settingsManager.settings.alpEnabled = true;
    settingsManager.settings.alpDisableV1LaserOnPush = false;
    settingsManager.setSlot(0, "Road", V1_MODE_UNKNOWN);

    profileManager.loadProfileResult = true;
    profileManager.loadableProfileName = "Road";
    profileManager.loadableProfile = makeProfile("Road", 0xFF);

    const auto result = module.queueSlotPush(0);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AutoPushModule::QueueResult::QUEUED),
                          static_cast<int>(result));

    runUntilIdle();

    TEST_ASSERT_EQUAL_INT(1, bleClient.writeUserBytesCalls);
    TEST_ASSERT_EQUAL_HEX8(0xFF, bleClient.lastUserBytes[0]);
}

void test_status_json_escapes_profile_name() {
    bleClient.setConnected(true);

    const String profileName = "Road\"A\\B\n";
    profileManager.loadProfileResult = true;
    profileManager.loadableProfileName = profileName;
    profileManager.loadableProfile = makeProfile("Escaped", 0xFF);

    AutoPushModule::PushNowRequest request;
    request.slotIndex = 0;
    request.hasProfileOverride = true;
    request.profileName = profileName;

    const auto result = module.queuePushNow(request);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AutoPushModule::QueueResult::QUEUED),
                          static_cast<int>(result));

    const String status = module.getStatusJson();
    TEST_ASSERT_NOT_NULL(std::strstr(
        status.c_str(),
        "\"profileName\":\"Road\\\"A\\\\B\\n\""));
}

void runAllTests() {
    RUN_TEST(test_start_runs_existing_auto_push_path);
    RUN_TEST(test_queue_push_now_with_profile_override_skips_slot_mode_without_override);
    RUN_TEST(test_queue_push_now_retries_display_step_and_tracks_push_now_metrics);
    RUN_TEST(test_start_while_active_does_not_override_inflight_request);
    RUN_TEST(test_queue_slot_push_can_suppress_immediate_profile_indicator_draw);
    RUN_TEST(test_alp_policy_disables_v1_laser_bit_on_profile_push);
    RUN_TEST(test_alp_policy_toggle_can_keep_v1_laser_bit_on_profile_push);
    RUN_TEST(test_status_json_escapes_profile_name);
}

#ifdef ARDUINO
void setup() {
    delay(2000);
    UNITY_BEGIN();
    runAllTests();
    UNITY_END();
}

void loop() {}
#else
int main(int argc, char** argv) {
    UNITY_BEGIN();
    runAllTests();
    return UNITY_END();
}
#endif
