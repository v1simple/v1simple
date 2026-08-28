#include <unity.h>

#include "../mocks/Arduino.h"
#include "../mocks/ble_client.h"
#include "../mocks/packet_parser.h"
#include "../mocks/settings.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#include "../../src/modules/volume_fade/volume_fade_module.cpp"
#include "../../src/modules/quiet/quiet_coordinator_module.cpp"
#include "../../src/modules/quiet/quiet_coordinator_templates.h"

namespace {
V1BLEClient ble;
PacketParser parser;
SettingsManager settings;
VolumeFadeModule fade;
QuietCoordinatorModule quiet;

void configureAlert() {
    settings.settings.alertVolumeFadeEnabled = true;
    settings.settings.alertVolumeFadeDelaySec = 2;
    settings.settings.alertVolumeFadeVolume = 1;
    parser.setMainVolume(6);
    parser.setMuteVolume(2);
    parser.setAlerts({AlertData::create(BAND_KA, DIR_FRONT, 6, 0, 34700, true, true)});
}
}

void setUp() {
    ble.reset();
    parser.reset();
    settings = SettingsManager{};
    fade = VolumeFadeModule{};
    quiet = QuietCoordinatorModule{};
    configureAlert();
    fade.begin(&settings);
    quiet.begin(&ble, &parser);
}

void tearDown() {}

static void assertOneTransientRetryCompletes() {
    TEST_ASSERT_FALSE(quiet.executeVolumeFade(1000, &fade));
    ble.nextVolumeSendResult = SendResult::NOT_YET;
    TEST_ASSERT_TRUE(quiet.executeVolumeFade(3500, &fade));
    TEST_ASSERT_EQUAL_INT(1, ble.setVolumeCalls);

    TEST_ASSERT_TRUE(quiet.executeVolumeFade(3510, &fade));
    TEST_ASSERT_EQUAL_INT(1, ble.setVolumeCalls);
    TEST_ASSERT_TRUE(quiet.executeVolumeFade(3525, &fade));
    TEST_ASSERT_EQUAL_INT(2, ble.setVolumeCalls);
    TEST_ASSERT_EQUAL_UINT8(1, ble.lastVolume);
    TEST_ASSERT_EQUAL_UINT8(2, ble.lastMuteVolume);

    TEST_ASSERT_FALSE(quiet.executeVolumeFade(3600, &fade));
    TEST_ASSERT_EQUAL_INT(2, ble.setVolumeCalls);
}

void test_one_time_pacing_not_yet_retries_without_spin_or_resend_after_success() {
    assertOneTransientRetryCompletes();
}

void test_one_time_write_busy_not_yet_retries_without_losing_fade() {
    assertOneTransientRetryCompletes();
}

void test_pending_fade_is_replaced_by_restore_when_new_alert_arrives() {
    TEST_ASSERT_FALSE(quiet.executeVolumeFade(1000, &fade));
    ble.nextVolumeSendResult = SendResult::NOT_YET;
    TEST_ASSERT_TRUE(quiet.executeVolumeFade(3500, &fade));
    TEST_ASSERT_EQUAL_UINT8(1, ble.lastVolume);

    parser.setAlerts({AlertData::create(BAND_KA, DIR_FRONT, 6, 0, 35500, true, true)});
    TEST_ASSERT_TRUE(quiet.executeVolumeFade(3510, &fade));
    TEST_ASSERT_EQUAL_INT(2, ble.setVolumeCalls);
    TEST_ASSERT_EQUAL_UINT8(6, ble.lastVolume);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_one_time_pacing_not_yet_retries_without_spin_or_resend_after_success);
    RUN_TEST(test_one_time_write_busy_not_yet_retries_without_losing_fade);
    RUN_TEST(test_pending_fade_is_replaced_by_restore_when_new_alert_arrives);
    return UNITY_END();
}
