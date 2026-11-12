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
#include "../mocks/runtime_presentation_source.h"
#include "../../src/touch_handler.cpp"

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
RuntimePresentationSource gPresentation{gSettings};
TouchUiModule gModule;

int gRestoreDisplayCalls = 0;
void restoreDisplay(void* /*ctx*/) { ++gRestoreDisplayCalls; }
int gMaintenanceBootCalls = 0;
uint8_t gBrightnessAtMaintenanceRequest = 0;
int gPersistCallsAtMaintenanceRequest = 0;
ObdRuntimeStatus gObdStatus;
bool gObdSafe = true;
int gObdPairRequests = 0;
void requestMaintenanceBoot(void* /*ctx*/) {
    ++gMaintenanceBootCalls;
    gBrightnessAtMaintenanceRequest = gSettings.settings.brightness;
    gPersistCallsAtMaintenanceRequest = gSettings.saveDeferredBackupCalls;
}

bool processInput(unsigned long now, bool bootPressed) {
    mockMillis = now;
    return gModule.process(now, bootPressed);
}

void queueTouch(uint16_t x, uint16_t y, uint8_t points = 1) {
    std::vector<uint8_t> data(32, 0);
    data[1] = points;
    data[2] = x >> 8;
    data[3] = x;
    data[4] = y >> 8;
    data[5] = y;
    Wire.queueRequestFrom(data.size(), data);
}

void queueTouchEvent(uint8_t event, uint16_t x, uint16_t y) {
    std::vector<uint8_t> data(32, 0);
    data[1] = 1;
    data[2] = static_cast<uint8_t>((event << 6) | (x >> 8));
    data[3] = x;
    data[4] = y >> 8;
    data[5] = y;
    Wire.queueRequestFrom(data.size(), data);
}

// Short BOOT press/release: the module's documented enter-adjust gesture.
unsigned long enterAdjustMode(unsigned long now) {
    processInput(now, true);
    now += 400; // >= BOOT_DEBOUNCE_MS, < MAINTENANCE_BOOT_LONG_PRESS_MS
    processInput(now, false);
    return now;
}

} // namespace

void setUp() {
    gModule = TouchUiModule{};
    gDisplay.reset();
    mockMillis = 0;
    Wire.resetMock();
    gTouch = TouchHandler{};
    TEST_ASSERT_TRUE(gTouch.begin());
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
    gObdStatus = ObdRuntimeStatus{};
    gObdSafe = true;
    gObdPairRequests = 0;

    TouchUiModule::Callbacks cbs{};
    cbs.restoreDisplay = &restoreDisplay;
    cbs.requestMaintenanceBoot = &requestMaintenanceBoot;
    cbs.readObdStatus = [](uint32_t, void*) { return gObdStatus; };
    cbs.isObdPairGestureSafe = [](uint32_t, void*) { return gObdSafe; };
    cbs.requestObdManualPairScan = [](uint32_t, void*) {
        ++gObdPairRequests;
        return true;
    };
    gModule.begin(&gDisplay, &gTouch, &gSettings, cbs, gPresentation);
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
    TEST_ASSERT_TRUE(processInput(now + 10, false)); // sliders own the loop

    TEST_ASSERT_TRUE(gModule.preemptForLiveAlert());

    // The session ended like a user exit, except persistence is deferred:
    // adjustments retained, sliders hidden, and live view restored so the alert
    // renders this same loop.
    TEST_ASSERT_EQUAL(1, gDisplay.hideBrightnessSliderCalls);
    TEST_ASSERT_EQUAL(1, gRestoreDisplayCalls);
    TEST_ASSERT_EQUAL(0, gSettings.saveDeferredBackupCalls);
    TEST_ASSERT_EQUAL(0, gSettings.requestDeferredPersistCalls);
    TEST_ASSERT_EQUAL(180, gSettings.settings.brightness);
    TEST_ASSERT_EQUAL(60, gSettings.settings.voiceVolume);
    TEST_ASSERT_EQUAL(0, gAudioSetVolumeCalls);

    // The menu stays closed: the loop is no longer consumed, and nothing
    // re-enters adjust mode without a fresh user gesture.
    TEST_ASSERT_FALSE(processInput(now + 20, false));
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
    TEST_ASSERT_EQUAL(0, gSettings.requestDeferredPersistCalls);
}

void test_maintenance_long_press_saves_active_slider_edits_before_request() {
    const unsigned long now = enterAdjustMode(1000);
    gDisplay.activeSliderFromTouch = 0;
    queueTouch(600, 100);
    TEST_ASSERT_TRUE(processInput(now + 100, false));

    TEST_ASSERT_TRUE(processInput(now + 200, true));
    TEST_ASSERT_FALSE(processInput(now + 4200, false));

    TEST_ASSERT_EQUAL(1, gMaintenanceBootCalls);
    TEST_ASSERT_EQUAL(80, gBrightnessAtMaintenanceRequest);
    TEST_ASSERT_EQUAL(1, gPersistCallsAtMaintenanceRequest);
    TEST_ASSERT_EQUAL(80, gSettings.settings.brightness);
    TEST_ASSERT_EQUAL(1, gSettings.saveDeferredBackupCalls);
    TEST_ASSERT_EQUAL(0, gSettings.requestDeferredPersistCalls);
}

void test_invalid_coordinates_cannot_edit_slider_values() {
    const unsigned long now = enterAdjustMode(1000);
    gDisplay.activeSliderFromTouch = 0;
    queueTouch(514, 514);
    TEST_ASSERT_TRUE(processInput(now + 100, false));
    TEST_ASSERT_FALSE(gTouch.isTouchActive());
    TEST_ASSERT_EQUAL_INT(0, gDisplay.updateSettingsSlidersCalls);
    TEST_ASSERT_TRUE(gModule.preemptForLiveAlert());
    TEST_ASSERT_EQUAL_UINT8(180, gSettings.get().brightness);
    TEST_ASSERT_EQUAL_UINT8(60, gSettings.get().voiceVolume);
}

void test_released_boot_never_requests_maintenance() {
    for (unsigned long now = 1000; now <= 12000; now += 100) {
        processInput(now, false);
    }
    TEST_ASSERT_EQUAL(0, gMaintenanceBootCalls);
    TEST_ASSERT_EQUAL(0, gDisplay.showSettingsSlidersCalls);
}

void test_boot_hold_requests_maintenance_only_once_on_release() {
    processInput(1000, true);
    processInput(5000, true); // Four seconds held: keep waiting for release.
    TEST_ASSERT_EQUAL(0, gMaintenanceBootCalls);

    processInput(5000, false); // Release at the exact four-second boundary.
    TEST_ASSERT_EQUAL(1, gMaintenanceBootCalls);
    processInput(7000, false);
    TEST_ASSERT_EQUAL(1, gMaintenanceBootCalls);
}

void test_boot_release_below_four_seconds_keeps_short_press_behavior() {
    processInput(1000, true);
    processInput(4999, false);
    TEST_ASSERT_EQUAL(0, gMaintenanceBootCalls);
    TEST_ASSERT_EQUAL(1, gDisplay.showSettingsSlidersCalls);
}

void test_eligible_ten_second_boot_hold_pairs_obd_instead_of_maintenance() {
    gObdStatus.enabled = true;

    processInput(1000, true);
    processInput(5000, true);
    processInput(11000, true);
    TEST_ASSERT_EQUAL(0, gMaintenanceBootCalls);
    TEST_ASSERT_EQUAL(0, gObdPairRequests);
    processInput(11001, false);
    TEST_ASSERT_EQUAL(0, gMaintenanceBootCalls);
    TEST_ASSERT_EQUAL(1, gObdPairRequests);
}

void test_interrupted_boot_requires_release_before_any_new_gesture() {
    for (unsigned long resumedHold : {300UL, 4000UL, 10002UL}) {
        setUp();
        gObdStatus.enabled = true;
        processInput(1000, true);
        gModule.suspendForPresentationOwner();
        processInput(3000, true);
        processInput(3000 + resumedHold, false);
        TEST_ASSERT_EQUAL_INT(0, gDisplay.showSettingsSlidersCalls);
        TEST_ASSERT_EQUAL_INT(0, gMaintenanceBootCalls);
        TEST_ASSERT_EQUAL_INT(0, gObdPairRequests);
        TEST_ASSERT_FALSE(gSettings.get().stealthEnabled);

        enterAdjustMode(4000 + resumedHold);
        TEST_ASSERT_EQUAL_INT(1, gDisplay.showSettingsSlidersCalls);
    }
}

void test_warning_restores_settings_while_canceled_boot_remains_held() {
    enterAdjustMode(1000);
    gDisplay.activeSliderFromTouch = 0;
    processInput(1600, true);
    gModule.suspendForPresentationOwner();
    TEST_ASSERT_TRUE(gModule.restorePresentationIfOwned());
    TEST_ASSERT_EQUAL_INT(2, gDisplay.showSettingsSlidersCalls);
    queueTouch(600, 100);
    TEST_ASSERT_TRUE(processInput(3000, true));
    TEST_ASSERT_EQUAL_INT(0, gDisplay.updateSettingsSlidersCalls);
    TEST_ASSERT_TRUE(processInput(7000, false));
    TEST_ASSERT_EQUAL_INT(0, gMaintenanceBootCalls);
    TEST_ASSERT_EQUAL_INT(0, gDisplay.hideBrightnessSliderCalls);
    // An observed release restores screen input with its existing debounce.
    queueTouch(600, 100);
    TEST_ASSERT_TRUE(processInput(7200, false));
    TEST_ASSERT_EQUAL_INT(1, gDisplay.updateSettingsSlidersCalls);
}

void test_settings_entry_and_both_exit_paths_cancel_existing_screen_contact() {
    for (bool preempt : {false, true}) {
        setUp();
        gDisplay.activeSliderFromTouch = 0;
        processInput(1000, true);
        queueTouch(600, 100); // Contact already down when settings takes ownership.
        TEST_ASSERT_TRUE(processInput(1400, false));
        TEST_ASSERT_FALSE(gTouch.isTouchActive());
        TEST_ASSERT_EQUAL_INT(0, gDisplay.updateSettingsSlidersCalls);
        queueTouch(0, 0, 0);
        processInput(1600, false);
        queueTouch(600, 100);
        processInput(1800, false);
        TEST_ASSERT_EQUAL_INT(1, gDisplay.updateSettingsSlidersCalls);

        if (preempt) {
            TEST_ASSERT_TRUE(gModule.preemptForLiveAlert());
        } else {
            queueTouch(600, 100);
            processInput(2200, true);
            TEST_ASSERT_FALSE(processInput(2600, false));
        }
        int16_t x, y;
        mockMillis = 2800;
        queueTouch(600, 100);
        TEST_ASSERT_FALSE(gTouch.getTouchPoint(x, y));
        TEST_ASSERT_FALSE(gTouch.isTouchActive());
        mockMillis = 3000;
        queueTouch(0, 0, 0);
        TEST_ASSERT_FALSE(gTouch.getTouchPoint(x, y));
        mockMillis = 3200;
        queueTouch(100, 100);
        TEST_ASSERT_TRUE(gTouch.getTouchPoint(x, y));
    }
}

void test_obd_release_crossing_threshold_uses_release_duration() {
    gObdStatus.enabled = true;
    processInput(1000, true);
    processInput(10999, true);
    processInput(11002, false);
    TEST_ASSERT_EQUAL_INT(1, gObdPairRequests);
    TEST_ASSERT_EQUAL_INT(0, gMaintenanceBootCalls);
}

void test_obd_release_uses_current_safety_and_status_with_maintenance_fallback() {
    for (bool loseSafety : {false, true}) {
        setUp();
        gObdStatus.enabled = true;
        processInput(1000, true);
        processInput(11000, true);
        TEST_ASSERT_TRUE(gDisplay.lastPresentationStatus.obdAttention);
        if (loseSafety) {
            gObdSafe = false;
        } else {
            gObdStatus.connected = true;
        }
        processInput(11002, false);
        TEST_ASSERT_EQUAL_INT(0, gObdPairRequests);
        TEST_ASSERT_EQUAL_INT(1, gMaintenanceBootCalls);
        TEST_ASSERT_FALSE(gDisplay.lastPresentationStatus.obdAttention);
    }
}

void test_ten_second_hold_in_settings_keeps_maintenance_save_fallback() {
    gObdStatus.enabled = true;
    enterAdjustMode(1000);
    gDisplay.activeSliderFromTouch = 0;
    queueTouch(600, 100);
    TEST_ASSERT_TRUE(processInput(1700, false));
    processInput(2000, true);
    processInput(12000, true);
    processInput(12002, false);
    TEST_ASSERT_EQUAL_INT(0, gObdPairRequests);
    TEST_ASSERT_EQUAL_INT(1, gMaintenanceBootCalls);
    TEST_ASSERT_EQUAL_INT(1, gSettings.saveDeferredBackupCalls);
}

void test_latest_brightness_is_applied_when_boot_exits_before_repaint() {
    enterAdjustMode(1000);
    gDisplay.activeSliderFromTouch = 0;
    processInput(1800, true);
    queueTouch(40, 100);
    TEST_ASSERT_TRUE(processInput(2170, true));
    queueTouch(600, 100);
    TEST_ASSERT_TRUE(processInput(2180, true));
    TEST_ASSERT_EQUAL_INT(1, gDisplay.updateSettingsSlidersCalls);
    TEST_ASSERT_FALSE(processInput(2200, false));
    TEST_ASSERT_EQUAL_INT(1, gDisplay.updateSettingsSlidersCalls);
    TEST_ASSERT_EQUAL_UINT8(80, gDisplay.lastBrightness);
    TEST_ASSERT_EQUAL_UINT8(80, gSettings.get().brightness);
    TEST_ASSERT_EQUAL_INT(1, gSettings.saveDeferredBackupCalls);
}

void test_warning_restores_latest_slider_brightness_before_repaint() {
    enterAdjustMode(1000);
    gDisplay.activeSliderFromTouch = 0;
    queueTouch(40, 100);
    TEST_ASSERT_TRUE(processInput(1505, false));
    queueTouch(600, 100);
    TEST_ASSERT_TRUE(processInput(1525, false));
    TEST_ASSERT_EQUAL_INT(1, gDisplay.updateSettingsSlidersCalls);
    gModule.suspendForPresentationOwner();
    gDisplay.setBrightness(255); // Warning temporarily owns PWM.
    TEST_ASSERT_TRUE(gModule.restorePresentationIfOwned());
    TEST_ASSERT_EQUAL_UINT8(80, gDisplay.lastBrightness);
    TEST_ASSERT_EQUAL_UINT8(80, gDisplay.lastSettingsBrightness);
    TEST_ASSERT_EQUAL_UINT8(180, gSettings.get().brightness);
}

void test_short_and_double_boot_thresholds_are_unchanged() {
    processInput(1000, true);
    processInput(1299, false);
    TEST_ASSERT_EQUAL_INT(0, gDisplay.showSettingsSlidersCalls);
    processInput(1500, true);
    processInput(1800, false);
    TEST_ASSERT_EQUAL_INT(1, gDisplay.showSettingsSlidersCalls);
    processInput(2099, true);
    processInput(2399, false); // Second release 599 ms later: double.
    TEST_ASSERT_TRUE(gSettings.get().stealthEnabled);
    TEST_ASSERT_EQUAL_INT(1, gDisplay.hideBrightnessSliderCalls);

    setUp();
    processInput(1500, true);
    processInput(1800, false);
    processInput(2100, true);
    processInput(2400, false); // At 600 ms this is an ordinary settings exit.
    TEST_ASSERT_FALSE(gSettings.get().stealthEnabled);
    TEST_ASSERT_EQUAL_INT(1, gDisplay.hideBrightnessSliderCalls);
}


// Independent literal drag regression: endpoints match the published physical
// slider range. Initial points are the unchanged current knob positions.
void dragGesture(int slider, int knobX, int16_t y, bool liftBeforeMove) {
    const unsigned long now = enterAdjustMode(1000);
    gDisplay.activeSliderFromTouch = slider;
    queueTouch(0, 0, 0);
    TEST_ASSERT_TRUE(processInput(now + 100, false));
    queueTouch(knobX, y);
    TEST_ASSERT_TRUE(processInput(now + 400, false));
    TEST_ASSERT_TRUE(gTouch.isTouchActive());
    if (liftBeforeMove) {
        queueTouch(0, 0, 0);
        TEST_ASSERT_TRUE(processInput(now + 700, false));
        TEST_ASSERT_FALSE(gTouch.isTouchActive());
    }
    queueTouch(600, y);
    TEST_ASSERT_TRUE(processInput(now + 1000, false));
    TEST_ASSERT_TRUE(gTouch.isTouchActive());
    TEST_ASSERT_TRUE(gModule.preemptForLiveAlert());
}
void test_brightness_held_drag_updates_from_existing_knob() {
    dragGesture(0, 280, 40, false);
    TEST_ASSERT_EQUAL_UINT8(80, gSettings.get().brightness);
}
void test_speaker_held_drag_updates_from_existing_knob() {
    dragGesture(1, 264, 120, false);
    TEST_ASSERT_EQUAL_UINT8(0, gSettings.get().voiceVolume);
    TEST_ASSERT_EQUAL_UINT8(0, gLastAudioVolume);
}
void test_brightness_separate_tap_control_updates() {
    dragGesture(0, 280, 40, true);
    TEST_ASSERT_EQUAL_UINT8(80, gSettings.get().brightness);
}
void test_speaker_separate_tap_control_updates() {
    dragGesture(1, 264, 120, true);
    TEST_ASSERT_EQUAL_UINT8(0, gSettings.get().voiceVolume);
    TEST_ASSERT_EQUAL_UINT8(0, gLastAudioVolume);
}

void test_slider_held_invalid_or_failed_poll_cannot_change_values() {
    const unsigned long now = enterAdjustMode(1000);
    gDisplay.activeSliderFromTouch = 1;
    queueTouch(0, 0, 0);
    processInput(now + 100, false);
    queueTouch(264, 120);
    processInput(now + 400, false);
    TEST_ASSERT_TRUE(gTouch.isTouchActive());
    queueTouch(600, 514);
    processInput(now + 700, false);
    TEST_ASSERT_FALSE(gTouch.isTouchActive());
    TEST_ASSERT_EQUAL_INT(60, gDisplay.lastSettingsVolume);
    TEST_ASSERT_EQUAL_INT(0, gAudioSetVolumeCalls);
    Wire.queueEndTransmission(2);
    processInput(now + 1000, false);
    TEST_ASSERT_FALSE(gTouch.isTouchActive());
    TEST_ASSERT_EQUAL_INT(60, gDisplay.lastSettingsVolume);
    TEST_ASSERT_EQUAL_INT(0, gAudioSetVolumeCalls);
    TEST_ASSERT_TRUE(gModule.preemptForLiveAlert());
    TEST_ASSERT_EQUAL_UINT8(60, gSettings.get().voiceVolume);
}
void test_stationary_slider_contact_keeps_existing_test_voice_debounce() {
    const unsigned long now = enterAdjustMode(1000);
    gDisplay.activeSliderFromTouch = 1;
    queueTouch(0, 0, 0);
    processInput(now + 100, false);
    queueTouch(320, 120);
    processInput(now + 400, false);
    TEST_ASSERT_EQUAL_INT(50, gDisplay.lastSettingsVolume);
    queueTouch(320, 120);
    processInput(now + 1399, false);
    TEST_ASSERT_EQUAL_INT(0, gPlayTestVoiceCalls);
    queueTouch(320, 120);
    processInput(now + 1400, false);
    TEST_ASSERT_TRUE(gTouch.isTouchActive());
    TEST_ASSERT_EQUAL_INT(1, gPlayTestVoiceCalls);
}

void rapidDragThenWait(bool released) {
    const unsigned long now = enterAdjustMode(1000);
    gDisplay.activeSliderFromTouch = 1;
    queueTouch(0, 0, 0);
    processInput(now + 100, false);
    queueTouch(320, 120);
    processInput(now + 400, false);
    TEST_ASSERT_EQUAL_INT(50, gDisplay.lastSettingsVolume);
    TEST_ASSERT_EQUAL_INT(1, gDisplay.updateSettingsSlidersCalls);
    queueTouch(600, 120);
    processInput(now + 420, false);
    TEST_ASSERT_EQUAL_UINT8(0, gLastAudioVolume);
    TEST_ASSERT_EQUAL_INT(50, gDisplay.lastSettingsVolume);
    TEST_ASSERT_EQUAL_INT(1, gDisplay.updateSettingsSlidersCalls);
    queueTouch(600, 120, released ? 0 : 1);
    processInput(now + 449, false);
    TEST_ASSERT_EQUAL_INT(1, gDisplay.updateSettingsSlidersCalls);
    queueTouch(600, 120, released ? 0 : 1);
    processInput(now + 450, false);
    TEST_ASSERT_EQUAL_INT(0, gDisplay.lastSettingsVolume);
    TEST_ASSERT_EQUAL_INT(2, gDisplay.updateSettingsSlidersCalls);
}
void test_rapid_drag_stationary_flushes_at_50ms() { rapidDragThenWait(false); }
void test_rapid_drag_release_flushes_at_50ms() { rapidDragThenWait(true); }

void test_up_coordinates_never_edit_sliders_and_next_down_saves() {
    enterAdjustMode(1000);
    gDisplay.activeSliderFromTouch = 1;
    queueTouchEvent(0, 320, 120);
    processInput(1800, false);
    TEST_ASSERT_EQUAL_INT(50, gDisplay.lastSettingsVolume);
    queueTouchEvent(1, 600, 120); // UP carries a different coordinate, not a new value.
    processInput(2050, false);
    TEST_ASSERT_FALSE(gTouch.isTouchActive());
    TEST_ASSERT_EQUAL_INT(50, gDisplay.lastSettingsVolume);
    TEST_ASSERT_EQUAL_INT(1, gAudioSetVolumeCalls);
    queueTouchEvent(0, 40, 120);
    processInput(2300, false);
    TEST_ASSERT_EQUAL_INT(100, gDisplay.lastSettingsVolume);
    queueTouchEvent(1, 40, 120);
    processInput(2550, false);
    // Explicit post-UP reports avoid the mock's implicit zero-count release.
    Wire.queueRequestFrom(32, std::vector<uint8_t>(32, 0x3c));
    processInput(3000, true);
    TEST_ASSERT_FALSE(processInput(3400, false));
    TEST_ASSERT_EQUAL_UINT8(100, gSettings.get().voiceVolume);
    TEST_ASSERT_EQUAL_INT(1, gSettings.saveDeferredBackupCalls);
    TEST_ASSERT_EQUAL_INT(1, gDisplay.hideBrightnessSliderCalls);
}

void test_up_unlocks_reopened_settings_without_carrying_a_held_touch() {
    processInput(1000, true);
    queueTouchEvent(1, 100, 100); // Release observed after settings takes ownership.
    TEST_ASSERT_TRUE(processInput(1400, false));
    gDisplay.activeSliderFromTouch = 0;
    queueTouchEvent(0, 600, 40);
    processInput(1800, false);
    TEST_ASSERT_EQUAL_INT(80, gDisplay.lastSettingsBrightness);
    queueTouchEvent(1, 600, 40);
    processInput(2050, false);
    Wire.queueRequestFrom(32, std::vector<uint8_t>(32, 0x3c));
    processInput(2500, true);
    TEST_ASSERT_FALSE(processInput(2900, false));
    TEST_ASSERT_EQUAL_UINT8(80, gSettings.get().brightness);
    TEST_ASSERT_EQUAL_INT(1, gSettings.saveDeferredBackupCalls);

    processInput(4000, true);
    queueTouchEvent(0, 40, 40); // A contact across entry remains canceled.
    TEST_ASSERT_TRUE(processInput(4400, false));
    TEST_ASSERT_FALSE(gTouch.isTouchActive());
    TEST_ASSERT_EQUAL_INT(80, gDisplay.lastSettingsBrightness);
    queueTouchEvent(1, 40, 40);
    processInput(4650, false);
    TEST_ASSERT_FALSE(gTouch.isTouchActive());
    queueTouchEvent(0, 40, 40);
    processInput(4750, false);
    TEST_ASSERT_EQUAL_INT(255, gDisplay.lastSettingsBrightness);
    queueTouchEvent(1, 40, 40);
    processInput(5000, false);
    Wire.queueRequestFrom(32, std::vector<uint8_t>(32, 0x3c));
    processInput(5500, true);
    TEST_ASSERT_FALSE(processInput(5900, false));
    TEST_ASSERT_EQUAL_UINT8(255, gSettings.get().brightness);
    TEST_ASSERT_EQUAL_INT(2, gSettings.saveDeferredBackupCalls);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_preempt_is_noop_when_menu_closed);
    RUN_TEST(test_preempt_closes_active_adjust_session);
    RUN_TEST(test_preempt_is_single_shot);
    RUN_TEST(test_latest_brightness_is_applied_when_boot_exits_before_repaint);
    RUN_TEST(test_warning_restores_latest_slider_brightness_before_repaint);
    RUN_TEST(test_maintenance_long_press_saves_active_slider_edits_before_request);
    RUN_TEST(test_released_boot_never_requests_maintenance);
    RUN_TEST(test_boot_hold_requests_maintenance_only_once_on_release);
    RUN_TEST(test_boot_release_below_four_seconds_keeps_short_press_behavior);
    RUN_TEST(test_eligible_ten_second_boot_hold_pairs_obd_instead_of_maintenance);
    RUN_TEST(test_invalid_coordinates_cannot_edit_slider_values);
    RUN_TEST(test_interrupted_boot_requires_release_before_any_new_gesture);
    RUN_TEST(test_warning_restores_settings_while_canceled_boot_remains_held);
    RUN_TEST(test_settings_entry_and_both_exit_paths_cancel_existing_screen_contact);
    RUN_TEST(test_obd_release_crossing_threshold_uses_release_duration);
    RUN_TEST(test_obd_release_uses_current_safety_and_status_with_maintenance_fallback);
    RUN_TEST(test_ten_second_hold_in_settings_keeps_maintenance_save_fallback);
    RUN_TEST(test_short_and_double_boot_thresholds_are_unchanged);
    RUN_TEST(test_brightness_held_drag_updates_from_existing_knob);
    RUN_TEST(test_speaker_held_drag_updates_from_existing_knob);
    RUN_TEST(test_brightness_separate_tap_control_updates);
    RUN_TEST(test_speaker_separate_tap_control_updates);
    RUN_TEST(test_slider_held_invalid_or_failed_poll_cannot_change_values);
    RUN_TEST(test_stationary_slider_contact_keeps_existing_test_voice_debounce);
    RUN_TEST(test_rapid_drag_stationary_flushes_at_50ms);
    RUN_TEST(test_rapid_drag_release_flushes_at_50ms);
    RUN_TEST(test_up_coordinates_never_edit_sliders_and_next_down_saves);
    RUN_TEST(test_up_unlocks_reopened_settings_without_carrying_a_held_touch);
    return UNITY_END();
}
