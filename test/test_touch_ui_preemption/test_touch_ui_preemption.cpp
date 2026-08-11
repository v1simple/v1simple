// TouchUiModule preemption — Valentine's Law surface.
//
// Screen/speaker contract (docs/VALENTINE_PHILOSOPHY.md): an alert owns the
// screen; the settings sliders may not hold it against a live alert, and the
// menu is not auto-restored when the alert clears. This suite pins the loop's
// preemption hook: preemptForLiveAlert() exits an active adjust session —
// retaining the in-progress adjustments, deferring persistence, and yielding
// the screen via the restore callback — and reports false when adjust mode is
// not active so the normal settings early-return path is unchanged.

#include <unity.h>

#include "../mocks/Arduino.h"
#include "../mocks/display.h"
#include "../mocks/settings.h"
#include "../mocks/touch_handler.h"

#ifndef ARDUINO
SerialClass Serial;
SettingsManager settingsManager;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

// The module cpp pulls the real src/audio_beep.h declarations (the mock
// settings guard suppresses the real settings header it includes). Stub the
// two entry points the module calls.
#include "../../src/modules/touch/touch_ui_module.cpp"

static int gAudioSetVolumeCalls = 0;
static uint8_t gLastAudioVolume = 0;
void audio_set_volume(uint8_t volumePercent) {
    ++gAudioSetVolumeCalls;
    gLastAudioVolume = volumePercent;
}
static int gPlayTestVoiceCalls = 0;
void play_test_voice() { ++gPlayTestVoiceCalls; }

namespace {

V1Display gDisplay;
TouchHandler gTouch;
SettingsManager gSettings;
TouchUiModule gModule;

int gRestoreDisplayCalls = 0;
void restoreDisplay(void* /*ctx*/) { ++gRestoreDisplayCalls; }

// Short BOOT press/release: the module's documented enter-adjust gesture.
unsigned long enterAdjustMode(unsigned long now) {
    gModule.process(now, true);
    now += 400; // >= BOOT_DEBOUNCE_MS, < MAINTENANCE_BOOT_LONG_PRESS_MS
    gModule.process(now, false);
    return now;
}

} // namespace

void setUp() {
    gModule = TouchUiModule{};
    gDisplay.reset();
    gTouch.reset();
    gSettings = SettingsManager{};
    gSettings.settings.brightness = 180;
    gSettings.settings.voiceVolume = 60;
    gRestoreDisplayCalls = 0;
    gAudioSetVolumeCalls = 0;
    gLastAudioVolume = 0;
    gPlayTestVoiceCalls = 0;

    TouchUiModule::Callbacks cbs{};
    cbs.restoreDisplay = &restoreDisplay;
    gModule.begin(&gDisplay, &gTouch, &gSettings, cbs);
}

void tearDown() {}

void test_preempt_is_noop_when_menu_closed() {
    TEST_ASSERT_FALSE(gModule.preemptForLiveAlert());
    TEST_ASSERT_EQUAL(0, gDisplay.hideBrightnessSliderCalls);
    TEST_ASSERT_EQUAL(0, gRestoreDisplayCalls);
    TEST_ASSERT_EQUAL(0, gSettings.saveDeferredBackupCalls);
    TEST_ASSERT_EQUAL(0, gSettings.requestDeferredPersistCalls);
}

void test_preempt_closes_active_adjust_session() {
    unsigned long now = enterAdjustMode(1000);
    TEST_ASSERT_EQUAL(1, gDisplay.showSettingsSlidersCalls);
    TEST_ASSERT_TRUE(gModule.process(now + 10, false)); // sliders own the loop

    TEST_ASSERT_TRUE(gModule.preemptForLiveAlert());

    // The session ended like a user exit, except persistence is deferred:
    // adjustments retained, sliders hidden, and live view restored so the alert
    // renders this same loop.
    TEST_ASSERT_EQUAL(1, gDisplay.hideBrightnessSliderCalls);
    TEST_ASSERT_EQUAL(1, gRestoreDisplayCalls);
    TEST_ASSERT_EQUAL(0, gSettings.saveDeferredBackupCalls);
    TEST_ASSERT_EQUAL(1, gSettings.requestDeferredPersistCalls);
    TEST_ASSERT_EQUAL(180, gSettings.settings.brightness);
    TEST_ASSERT_EQUAL(60, gSettings.settings.voiceVolume);
    TEST_ASSERT_EQUAL(60, gLastAudioVolume);

    // The menu stays closed: the loop is no longer consumed, and nothing
    // re-enters adjust mode without a fresh user gesture.
    TEST_ASSERT_FALSE(gModule.process(now + 20, false));
    TEST_ASSERT_EQUAL(1, gDisplay.showSettingsSlidersCalls);
}

void test_preempt_is_single_shot() {
    unsigned long now = enterAdjustMode(1000);
    (void)now;
    TEST_ASSERT_TRUE(gModule.preemptForLiveAlert());
    TEST_ASSERT_FALSE(gModule.preemptForLiveAlert());
    TEST_ASSERT_EQUAL(1, gDisplay.hideBrightnessSliderCalls);
    TEST_ASSERT_EQUAL(1, gRestoreDisplayCalls);
    TEST_ASSERT_EQUAL(0, gSettings.saveDeferredBackupCalls);
    TEST_ASSERT_EQUAL(1, gSettings.requestDeferredPersistCalls);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_preempt_is_noop_when_menu_closed);
    RUN_TEST(test_preempt_closes_active_adjust_session);
    RUN_TEST(test_preempt_is_single_shot);
    return UNITY_END();
}
