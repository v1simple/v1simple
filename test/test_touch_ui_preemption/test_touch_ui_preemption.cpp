// TouchUiModule preemption — Valentine's Law surface.
//
// Screen/speaker contract (docs/VALENTINE_PHILOSOPHY.md): an alert owns the
// screen; the settings sliders may not hold it against a live alert, and the
// menu is not auto-restored when the alert clears. This suite pins the loop's
// preemption hook: preemptForLiveAlert() exits an active adjust session —
// retaining the in-progress adjustments, deferring persistence, and yielding
// the screen via the restore callback — and reports false when adjust mode is
// not active so the normal settings early-return path is unchanged.
// Physical BOOT remains the maintenance-entry owner; its hold/release and
// longer OBD-pair gesture are exercised directly against the same module.

#include <unity.h>

#include "../mocks/Arduino.h"
#include "../mocks/display.h"
#include "../mocks/settings.h"
#include "../mocks/touch_handler.h"

#ifndef ARDUINO
SerialClass Serial;
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
int gMaintenanceBootCalls = 0;
uint8_t gBrightnessAtMaintenanceRequest = 0;
int gPersistCallsAtMaintenanceRequest = 0;
void requestMaintenanceBoot(void* /*ctx*/) {
    ++gMaintenanceBootCalls;
    gBrightnessAtMaintenanceRequest = gSettings.settings.brightness;
    gPersistCallsAtMaintenanceRequest = gSettings.saveDeferredBackupCalls;
}

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
    gMaintenanceBootCalls = 0;
    gBrightnessAtMaintenanceRequest = 0;
    gPersistCallsAtMaintenanceRequest = 0;

    TouchUiModule::Callbacks cbs{};
    cbs.restoreDisplay = &restoreDisplay;
    cbs.requestMaintenanceBoot = &requestMaintenanceBoot;
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

void test_maintenance_long_press_saves_active_slider_edits_before_request() {
    const unsigned long now = enterAdjustMode(1000);
    gDisplay.activeSliderFromTouch = 0;
    gTouch.queueTouch(600, 100);
    TEST_ASSERT_TRUE(gModule.process(now + 10, false));

    TEST_ASSERT_TRUE(gModule.process(now + 100, true));
    TEST_ASSERT_FALSE(gModule.process(now + 4200, false));

    TEST_ASSERT_EQUAL(1, gMaintenanceBootCalls);
    TEST_ASSERT_EQUAL(80, gBrightnessAtMaintenanceRequest);
    TEST_ASSERT_EQUAL(1, gPersistCallsAtMaintenanceRequest);
    TEST_ASSERT_EQUAL(80, gSettings.settings.brightness);
    TEST_ASSERT_EQUAL(1, gSettings.saveDeferredBackupCalls);
    TEST_ASSERT_EQUAL(0, gSettings.requestDeferredPersistCalls);
}

void test_released_boot_never_requests_maintenance() {
    for (unsigned long now = 1000; now <= 12000; now += 100) {
        gModule.process(now, false);
    }
    TEST_ASSERT_EQUAL(0, gMaintenanceBootCalls);
    TEST_ASSERT_EQUAL(0, gDisplay.showSettingsSlidersCalls);
}

void test_boot_hold_requests_maintenance_only_once_on_release() {
    gModule.process(1000, true);
    gModule.process(5000, true); // Four seconds held: keep waiting for release.
    TEST_ASSERT_EQUAL(0, gMaintenanceBootCalls);

    gModule.process(5000, false); // Release at the exact four-second boundary.
    TEST_ASSERT_EQUAL(1, gMaintenanceBootCalls);
    gModule.process(7000, false);
    TEST_ASSERT_EQUAL(1, gMaintenanceBootCalls);
}

void test_boot_release_below_four_seconds_keeps_short_press_behavior() {
    gModule.process(1000, true);
    gModule.process(4999, false);
    TEST_ASSERT_EQUAL(0, gMaintenanceBootCalls);
    TEST_ASSERT_EQUAL(1, gDisplay.showSettingsSlidersCalls);
}

void test_eligible_ten_second_boot_hold_pairs_obd_instead_of_maintenance() {
    int pairRequests = 0;
    TouchUiModule::Callbacks cbs{};
    cbs.requestMaintenanceBoot = &requestMaintenanceBoot;
    cbs.readObdStatus = [](uint32_t, void*) {
        ObdRuntimeStatus status;
        status.enabled = true;
        return status;
    };
    cbs.isObdPairGestureSafe = [](uint32_t, void*) { return true; };
    cbs.requestObdManualPairScan = [](uint32_t, void* ctx) {
        ++*static_cast<int*>(ctx);
        return true;
    };
    cbs.requestObdManualPairScanCtx = &pairRequests;
    gModule.begin(&gDisplay, &gTouch, &gSettings, cbs);

    gModule.process(1000, true);
    gModule.process(5000, true);
    gModule.process(11000, true);
    TEST_ASSERT_EQUAL(0, gMaintenanceBootCalls);
    TEST_ASSERT_EQUAL(0, pairRequests);
    gModule.process(11001, false);
    TEST_ASSERT_EQUAL(0, gMaintenanceBootCalls);
    TEST_ASSERT_EQUAL(1, pairRequests);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_preempt_is_noop_when_menu_closed);
    RUN_TEST(test_preempt_closes_active_adjust_session);
    RUN_TEST(test_preempt_is_single_shot);
    RUN_TEST(test_maintenance_long_press_saves_active_slider_edits_before_request);
    RUN_TEST(test_released_boot_never_requests_maintenance);
    RUN_TEST(test_boot_hold_requests_maintenance_only_once_on_release);
    RUN_TEST(test_boot_release_below_four_seconds_keeps_short_press_behavior);
    RUN_TEST(test_eligible_ten_second_boot_hold_pairs_obd_instead_of_maintenance);
    return UNITY_END();
}
