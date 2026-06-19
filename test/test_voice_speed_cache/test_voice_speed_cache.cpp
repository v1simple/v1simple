#include <unity.h>
#include <climits>

#include "../mocks/ble_client.h"
#include "../mocks/settings.h"
#include "../../src/modules/voice/voice_module.h"
#include "../../src/modules/voice/voice_module.cpp"  // Pull implementation for UNIT_TEST

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

SettingsManager settingsManager;
static VoiceModule voiceModule;
static V1BLEClient bleClient;

void setUp() {
    settingsManager = SettingsManager();
    bleClient.reset();
    voiceModule = VoiceModule();
    voiceModule.begin(&settingsManager, &bleClient);
}

void tearDown() {
}

void test_speed_sample_valid_within_ttl() {
    voiceModule.updateSpeedSample(42.5f, 1000);

    float speedMph = 0.0f;
    TEST_ASSERT_TRUE(voiceModule.getCurrentSpeedSample(1200, speedMph));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 42.5f, speedMph);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 42.5f, voiceModule.getCurrentSpeedMph(1200));
}

void test_speed_sample_expires_after_ttl() {
    voiceModule.updateSpeedSample(63.0f, 1000);

    float speedMph = 0.0f;
    TEST_ASSERT_FALSE(voiceModule.getCurrentSpeedSample(7000, speedMph));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, voiceModule.getCurrentSpeedMph(7000));
}

void test_invalid_sample_preserves_previous_cache() {
    voiceModule.updateSpeedSample(30.0f, 2000);
    voiceModule.updateSpeedSample(-1.0f, 2100);  // ignored

    float speedMph = 0.0f;
    TEST_ASSERT_TRUE(voiceModule.getCurrentSpeedSample(2500, speedMph));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 30.0f, speedMph);
}

void test_clear_speed_sample_invalidates_cache() {
    voiceModule.updateSpeedSample(48.0f, 1000);
    voiceModule.clearSpeedSample();

    float speedMph = 0.0f;
    TEST_ASSERT_FALSE(voiceModule.getCurrentSpeedSample(1200, speedMph));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, voiceModule.getCurrentSpeedMph(1200));
}

void test_alert_history_recycle_evicts_oldest_after_wraparound() {
    // Fill all alert history slots
    for (int i = 0; i < VoiceModule::TEST_MAX_ALERT_HISTORIES; i++) {
        // Use timestamps near ULONG_MAX to simulate pre-wraparound state
        unsigned long ts = (ULONG_MAX - 5000) + (i * 100);
        voiceModule.testUpdateAlertHistory(BAND_K, static_cast<uint16_t>(24100 + i), 3, ts);
    }
    TEST_ASSERT_EQUAL(VoiceModule::TEST_MAX_ALERT_HISTORIES, voiceModule.getAlertHistoryCount());

    // Now millis() has wrapped around to a small value
    unsigned long wrappedNow = 500;
    // Slot 0 has timestamp (ULONG_MAX - 5000) → oldest by elapsed time
    // Creating a new alert should evict slot 0 (the truly oldest)
    voiceModule.testUpdateAlertHistory(BAND_KA, 34700, 4, wrappedNow);

    // The new alert should have replaced the oldest slot
    // Verify the new alert exists in history
    bool foundNew = false;
    uint32_t newId = VoiceModule::makeAlertId(BAND_KA, 34700);
    auto& histories = voiceModule.getAlertHistories();
    uint8_t count = voiceModule.getAlertHistoryCount();
    for (int i = 0; i < count; i++) {
        if (histories[i].alertId == newId) {
            foundNew = true;
            TEST_ASSERT_EQUAL(wrappedNow, histories[i].lastUpdateMs);
            break;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(foundNew, "New alert should be in history after wraparound recycle");
}

void test_clear_all_state_allows_immediate_reannounce_after_all_clear() {
    settingsManager.settings.muteVoiceIfVolZero = false;

    AlertData alerts[1];
    alerts[0].band = BAND_KA;
    alerts[0].direction = DIR_FRONT;
    alerts[0].frontStrength = 6;
    alerts[0].rearStrength = 0;
    alerts[0].frequency = 35500;
    alerts[0].isValid = true;
    alerts[0].isPriority = true;

    VoiceContext firstCtx;
    firstCtx.alerts = alerts;
    firstCtx.alertCount = 1;
    firstCtx.priority = &alerts[0];
    firstCtx.mainVolume = 5;
    firstCtx.now = 100000;

    const VoiceAction firstAction = voiceModule.process(firstCtx);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(VoiceAction::Type::ANNOUNCE_PRIORITY),
                          static_cast<int>(firstAction.type));

    voiceModule.clearAllState();

    VoiceContext secondCtx = firstCtx;
    secondCtx.now = 101000;

    const VoiceAction secondAction = voiceModule.process(secondCtx);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(VoiceAction::Type::ANNOUNCE_PRIORITY),
                          static_cast<int>(secondAction.type));
}

void runAllTests() {
    RUN_TEST(test_speed_sample_valid_within_ttl);
    RUN_TEST(test_speed_sample_expires_after_ttl);
    RUN_TEST(test_invalid_sample_preserves_previous_cache);
    RUN_TEST(test_clear_speed_sample_invalidates_cache);
    RUN_TEST(test_alert_history_recycle_evicts_oldest_after_wraparound);
    RUN_TEST(test_clear_all_state_allows_immediate_reannounce_after_all_clear);
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
