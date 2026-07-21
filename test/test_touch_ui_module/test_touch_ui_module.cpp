#include <unity.h>

#include "../mocks/display.h"
#include "../mocks/settings.h"
#include "../mocks/touch_handler.h"
#include "../../src/modules/touch/touch_ui_module.cpp"

#ifndef ARDUINO
SerialClass Serial;
#endif

namespace {

int audioSetVolumeCalls = 0;
uint8_t lastAudioSetVolume = 0;
int playTestVoiceCalls = 0;

V1Display display;
TouchHandler touchHandler;
SettingsManager settingsManager;
TouchUiModule touchUiModule;

bool wifiSetupActive = false;
int wifiStartCalls = 0;
int wifiStopCalls = 0;
int maintenanceBootRequests = 0;
int manualPairRequests = 0;
bool obdPairSafe = true;
ObdRuntimeStatus obdStatus;

TouchUiModule::Callbacks makeCallbacks() {
    return TouchUiModule::Callbacks{
        .isWifiSetupActive = [](void* /*ctx*/) { return wifiSetupActive; },
        .stopWifiSetup = [](void* /*ctx*/) { ++wifiStopCalls; wifiSetupActive = false; },
        .requestMaintenanceBoot = [](void* /*ctx*/) { ++maintenanceBootRequests; },
        .drawWifiIndicator = [](void* /*ctx*/) { display.drawWiFiIndicator(); },
        .restoreDisplay = [](void* /*ctx*/) {},
        .readObdStatus = [](uint32_t, void* /*ctx*/) { return obdStatus; },
        .requestObdManualPairScan = [](uint32_t, void* /*ctx*/) {
            ++manualPairRequests;
            obdStatus.manualScanPending = true;
            return true;
        },
        .isObdPairGestureSafe = [](uint32_t, void* /*ctx*/) { return obdPairSafe; },
    };
}

void resetFixture() {
    display.reset();
    touchHandler.reset();
    settingsManager = SettingsManager();
    settingsManager.settings.brightness = 240;
    settingsManager.settings.voiceVolume = 70;
    audioSetVolumeCalls = 0;
    lastAudioSetVolume = 0;
    playTestVoiceCalls = 0;

    wifiSetupActive = false;
    wifiStartCalls = 0;
    wifiStopCalls = 0;
    maintenanceBootRequests = 0;
    manualPairRequests = 0;
    obdPairSafe = true;
    obdStatus = ObdRuntimeStatus{};

    touchUiModule = TouchUiModule();
    touchUiModule.begin(&display, &touchHandler, &settingsManager, makeCallbacks());
}

uint8_t fixtureVoiceVolume() {
    return settingsManager.settings.voiceVolume;
}

int fixtureSaveCalls() {
    return settingsManager.saveCalls;
}

int fixtureDeferredBackupCalls() {
    return settingsManager.saveDeferredBackupCalls;
}

bool fixtureStealthEnabled() {
    return settingsManager.settings.stealthEnabled;
}

}  // namespace

void audio_set_volume(uint8_t volumePercent) {
    ++audioSetVolumeCalls;
    lastAudioSetVolume = volumePercent;
}

void play_test_voice() {
    ++playTestVoiceCalls;
}

void play_vol0_beep() {}
void play_alert_voice(AlertBand, AlertDirection) {}
void play_frequency_voice(AlertBand, uint16_t, AlertDirection, VoiceAlertMode, bool, uint8_t) {}
void play_direction_only(AlertDirection, uint8_t) {}
void play_threat_escalation(AlertBand, uint16_t, AlertDirection,
                            uint8_t, uint8_t, uint8_t, uint8_t) {}
void play_band_only(AlertBand) {}
void audio_init_sd() {}
void audio_init_buffers() {}
void audio_process_amp_timeout() {}

void setUp() {
    resetFixture();
}

void tearDown() {}

void test_short_press_keeps_existing_settings_mode_behavior() {
    TEST_ASSERT_FALSE(touchUiModule.process(0, true));
    TEST_ASSERT_TRUE(touchUiModule.process(350, false));

    TEST_ASSERT_EQUAL_INT(1, display.showSettingsSlidersCalls);
    TEST_ASSERT_EQUAL_INT(240, display.lastSettingsBrightness);
    TEST_ASSERT_EQUAL_INT(70, display.lastSettingsVolume);
    TEST_ASSERT_EQUAL_INT(0, wifiStartCalls);
    TEST_ASSERT_EQUAL_INT(0, manualPairRequests);
    TEST_ASSERT_EQUAL_INT(0, display.setObdAttentionCalls);
}

void test_volume_slider_updates_audio_and_saves_voice_volume() {
    TEST_ASSERT_FALSE(touchUiModule.process(0, true));
    TEST_ASSERT_TRUE(touchUiModule.process(350, false));

    display.activeSliderFromTouch = 1;
    touchHandler.queueTouch(460, 45);
    TEST_ASSERT_TRUE(touchUiModule.process(500, false));

    TEST_ASSERT_EQUAL_INT(1, display.updateSettingsSlidersCalls);
    TEST_ASSERT_EQUAL_INT(240, display.lastSettingsBrightness);
    TEST_ASSERT_EQUAL_INT(25, display.lastSettingsVolume);
    TEST_ASSERT_EQUAL_INT(1, display.lastSettingsActiveSlider);
    TEST_ASSERT_EQUAL_INT(1, audioSetVolumeCalls);
    TEST_ASSERT_EQUAL_UINT8(25, lastAudioSetVolume);

    TEST_ASSERT_TRUE(touchUiModule.process(1500, false));
    TEST_ASSERT_EQUAL_INT(1, playTestVoiceCalls);

    TEST_ASSERT_TRUE(touchUiModule.process(1700, true));
    TEST_ASSERT_FALSE(touchUiModule.process(2050, false));

    TEST_ASSERT_EQUAL_UINT8(25, fixtureVoiceVolume());
    TEST_ASSERT_EQUAL_INT(0, fixtureSaveCalls());
    TEST_ASSERT_EQUAL_INT(1, fixtureDeferredBackupCalls());
    TEST_ASSERT_EQUAL_INT(2, audioSetVolumeCalls);
    TEST_ASSERT_EQUAL_UINT8(25, lastAudioSetVolume);
    TEST_ASSERT_EQUAL_INT(1, display.hideBrightnessSliderCalls);
}

void test_double_press_defers_both_adjustment_and_stealth_sd_backups() {
    TEST_ASSERT_FALSE(touchUiModule.process(0, true));
    TEST_ASSERT_TRUE(touchUiModule.process(350, false));

    TEST_ASSERT_TRUE(touchUiModule.process(400, true));
    TEST_ASSERT_FALSE(touchUiModule.process(750, false));

    TEST_ASSERT_TRUE(fixtureStealthEnabled());
    TEST_ASSERT_EQUAL_INT(0, fixtureSaveCalls());
    TEST_ASSERT_EQUAL_INT(2, fixtureDeferredBackupCalls());
}

void test_four_second_press_requests_maintenance_boot_on_release() {
    TEST_ASSERT_FALSE(touchUiModule.process(0, true));
    // Maintenance does not fire while held, preserving the 10s OBD gesture.
    TEST_ASSERT_FALSE(touchUiModule.process(4000, true));
    TEST_ASSERT_EQUAL_INT(0, maintenanceBootRequests);
    TEST_ASSERT_EQUAL_INT(0, wifiStartCalls);

    // Release after the threshold requests a maintenance reboot.
    TEST_ASSERT_FALSE(touchUiModule.process(4500, false));
    TEST_ASSERT_EQUAL_INT(1, maintenanceBootRequests);
    TEST_ASSERT_EQUAL_INT(0, wifiStartCalls);
    TEST_ASSERT_EQUAL_INT(0, manualPairRequests);
    TEST_ASSERT_EQUAL_INT(0, display.showSettingsSlidersCalls);
}

void test_four_second_press_stops_existing_wifi_instead_of_rebooting() {
    wifiSetupActive = true;

    TEST_ASSERT_FALSE(touchUiModule.process(0, true));
    TEST_ASSERT_FALSE(touchUiModule.process(4500, false));

    TEST_ASSERT_EQUAL_INT(0, maintenanceBootRequests);
    TEST_ASSERT_EQUAL_INT(1, wifiStopCalls);
    TEST_ASSERT_FALSE(wifiSetupActive);
    TEST_ASSERT_EQUAL_INT(1, display.drawWiFiIndicatorCalls);
    TEST_ASSERT_EQUAL_INT(1, display.flushCalls);
}

void test_ten_second_press_arms_obd_pair_without_maintenance_reboot() {
    obdStatus.enabled = true;

    TEST_ASSERT_FALSE(touchUiModule.process(0, true));
    TEST_ASSERT_FALSE(touchUiModule.process(3999, true));
    TEST_ASSERT_FALSE(touchUiModule.process(4000, true));
    TEST_ASSERT_EQUAL_INT(0, maintenanceBootRequests);

    // OBD arm fires at 10s
    TEST_ASSERT_FALSE(touchUiModule.process(10000, true));
    TEST_ASSERT_EQUAL_INT(1, display.setObdAttentionCalls);
    TEST_ASSERT_TRUE(display.lastObdAttention);
    TEST_ASSERT_EQUAL_INT(1, display.drawObdIndicatorCalls);
    TEST_ASSERT_EQUAL_INT(1, display.flushRegionCalls);

    TEST_ASSERT_FALSE(touchUiModule.process(10050, false));

    TEST_ASSERT_EQUAL_INT(1, manualPairRequests);
    TEST_ASSERT_EQUAL_INT(0, maintenanceBootRequests);
    TEST_ASSERT_EQUAL_INT(2, display.setObdAttentionCalls);
    TEST_ASSERT_FALSE(display.lastObdAttention);
}

void test_ten_second_press_requests_maintenance_when_obd_pair_not_eligible() {
    obdStatus.enabled = true;
    obdStatus.connected = true;

    TEST_ASSERT_FALSE(touchUiModule.process(0, true));
    TEST_ASSERT_FALSE(touchUiModule.process(4000, true));
    TEST_ASSERT_EQUAL_INT(0, maintenanceBootRequests);

    TEST_ASSERT_FALSE(touchUiModule.process(10000, true));
    TEST_ASSERT_FALSE(touchUiModule.process(10050, false));

    TEST_ASSERT_EQUAL_INT(0, manualPairRequests);
    TEST_ASSERT_EQUAL_INT(1, maintenanceBootRequests);
    TEST_ASSERT_EQUAL_INT(0, display.setObdAttentionCalls);
}

void test_obd_pair_arm_clears_when_safety_disappears_before_release() {
    obdStatus.enabled = true;

    TEST_ASSERT_FALSE(touchUiModule.process(0, true));
    TEST_ASSERT_FALSE(touchUiModule.process(4000, true));
    TEST_ASSERT_EQUAL_INT(0, maintenanceBootRequests);

    TEST_ASSERT_FALSE(touchUiModule.process(10000, true));
    TEST_ASSERT_TRUE(display.lastObdAttention);

    obdPairSafe = false;
    TEST_ASSERT_FALSE(touchUiModule.process(10001, true));
    TEST_ASSERT_FALSE(display.lastObdAttention);

    TEST_ASSERT_FALSE(touchUiModule.process(10050, false));

    TEST_ASSERT_EQUAL_INT(0, manualPairRequests);
    TEST_ASSERT_EQUAL_INT(1, maintenanceBootRequests);
}

int main() {
    UNITY_BEGIN();

    RUN_TEST(test_short_press_keeps_existing_settings_mode_behavior);
    RUN_TEST(test_volume_slider_updates_audio_and_saves_voice_volume);
    RUN_TEST(test_double_press_defers_both_adjustment_and_stealth_sd_backups);
    RUN_TEST(test_four_second_press_requests_maintenance_boot_on_release);
    RUN_TEST(test_four_second_press_stops_existing_wifi_instead_of_rebooting);
    RUN_TEST(test_ten_second_press_arms_obd_pair_without_maintenance_reboot);
    RUN_TEST(test_ten_second_press_requests_maintenance_when_obd_pair_not_eligible);
    RUN_TEST(test_obd_pair_arm_clears_when_safety_disappears_before_release);

    return UNITY_END();
}
