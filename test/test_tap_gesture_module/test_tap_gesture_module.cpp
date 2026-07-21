#include <unity.h>

#include "../mocks/Arduino.h"
#include "../mocks/settings.h"
#include "../mocks/display.h"
#include "../mocks/ble_client.h"
#include "../mocks/packet_parser.h"
#include "../mocks/touch_handler.h"
#include "../mocks/modules/auto_push/auto_push_module.h"
#include "../mocks/modules/alert_persistence/alert_persistence_module.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

SettingsManager settingsManager;

#include "../../src/modules/quiet/quiet_coordinator_module.cpp"
#include "../../src/modules/touch/tap_gesture_module.cpp"

namespace {

V1Display display;
V1BLEClient bleClient;
PacketParser parser;
TouchHandler touch;
AutoPushModule autoPush;
AlertPersistenceModule alertPersistence;
DisplayMode displayMode = DisplayMode::LIVE;
QuietCoordinatorModule quiet;
TapGestureModule module;
bool wifiActive = false;
int wifiStopCalls = 0;
int maintenanceBootRequests = 0;

AlertData makeAlert() {
    return AlertData::create(BAND_KA, DIR_FRONT, 4, 0, 34520, true, true);
}

void setTime(unsigned long nowMs) {
    mockMillis = nowMs;
    mockMicros = nowMs * 1000;
}

void processAt(unsigned long nowMs) {
    setTime(nowMs);
    module.process(nowMs);
}

}  // namespace

void setUp() {
    setTime(0);
    ::settingsManager = SettingsManager{};
    ::settingsManager.settings.activeSlot = 0;
    ::settingsManager.settings.autoPushEnabled = false;
    display.reset();
    bleClient.reset();
    parser.reset();
    touch.reset();
    autoPush.reset();
    alertPersistence.reset();
    displayMode = DisplayMode::LIVE;
    wifiActive = false;
    wifiStopCalls = 0;
    maintenanceBootRequests = 0;

    module = TapGestureModule{};
    quiet.begin(&bleClient, &parser);
    module.begin(
        &touch,
        &::settingsManager,
        &display,
        &bleClient,
        &parser,
        &autoPush,
        &alertPersistence,
        &displayMode,
        &quiet,
        TapGestureModule::WifiCallbacks{
            .isWifiActive = [](void*) { return wifiActive; },
            .stopWifi = [](void*) { ++wifiStopCalls; wifiActive = false; },
            .requestMaintenanceBoot = [](void*) { ++maintenanceBootRequests; },
        });
}

void tearDown() {}

void test_alert_tap_toggles_mute_immediately() {
    parser.setAlerts({makeAlert()});
    parser.state.muted = false;
    touch.queueTouch(40, 20);

    processAt(200);

    TEST_ASSERT_EQUAL(1, bleClient.setMuteCalls);
    TEST_ASSERT_TRUE(bleClient.lastMuteValue);
    TEST_ASSERT_EQUAL(0, autoPush.queueSlotPushCalls);
    TEST_ASSERT_EQUAL(0, display.drawProfileIndicatorCalls);
}

void test_idle_triple_tap_cycles_slot_and_pushes_when_connected() {
    ::settingsManager.settings.autoPushEnabled = true;
    bleClient.setConnected(true);
    // Real taps have releases between them (the driver is edge-triggered and
    // requires a release before the next tap registers).
    touch.queueTouch(10, 10);
    touch.queueNoTouch();
    touch.queueTouch(10, 10);
    touch.queueNoTouch();
    touch.queueTouch(10, 10);

    processAt(200);
    TEST_ASSERT_EQUAL(0, display.drawProfileIndicatorCalls);
    TEST_ASSERT_EQUAL(0, autoPush.queueSlotPushCalls);

    processAt(300);
    processAt(400);
    TEST_ASSERT_EQUAL(0, display.drawProfileIndicatorCalls);
    TEST_ASSERT_EQUAL(0, autoPush.queueSlotPushCalls);

    processAt(500);
    processAt(600);

    TEST_ASSERT_EQUAL(1, ::settingsManager.settings.activeSlot);
    TEST_ASSERT_EQUAL(DisplayMode::IDLE, displayMode);
    TEST_ASSERT_EQUAL(1, alertPersistence.clearPersistenceCalls);
    TEST_ASSERT_EQUAL(1, display.drawProfileIndicatorCalls);
    TEST_ASSERT_EQUAL(1, display.lastProfileIndicatorSlot);
    TEST_ASSERT_EQUAL(1, autoPush.queueSlotPushCalls);
    TEST_ASSERT_EQUAL(1, autoPush.lastQueueSlotPushSlot);
    TEST_ASSERT_EQUAL(0, ::settingsManager.saveCalls);
    TEST_ASSERT_EQUAL(1, ::settingsManager.saveDeferredBackupCalls);
}

void test_idle_profile_cycle_resets_after_tap_window_expires() {
    touch.queueTouch(10, 10);
    touch.queueNoTouch();
    touch.queueTouch(10, 10);
    touch.queueNoTouch();
    touch.queueTouch(10, 10);

    processAt(200);
    processAt(300);
    processAt(400);
    processAt(500);
    processAt(1201);

    TEST_ASSERT_EQUAL(0, display.drawProfileIndicatorCalls);
    TEST_ASSERT_EQUAL(0, autoPush.queueSlotPushCalls);
    TEST_ASSERT_EQUAL(0, ::settingsManager.settings.activeSlot);
}

void test_long_press_requests_maintenance_boot_instead_of_starting_wifi() {
    touch.queueTouch(20, 20);
    touch.queueTouch(20, 20);

    processAt(0);
    processAt(4000);

    TEST_ASSERT_EQUAL(1, maintenanceBootRequests);
    TEST_ASSERT_EQUAL(0, wifiStopCalls);
}

void test_long_press_stops_wifi_if_already_active() {
    wifiActive = true;
    touch.queueTouch(20, 20);
    touch.queueTouch(20, 20);

    processAt(0);
    processAt(4000);

    TEST_ASSERT_EQUAL(0, maintenanceBootRequests);
    TEST_ASSERT_EQUAL(1, wifiStopCalls);
    TEST_ASSERT_FALSE(wifiActive);
}

void test_long_press_below_threshold_does_not_fire() {
    touch.queueTouch(20, 20);
    touch.queueTouch(20, 20);  // held

    processAt(0);
    processAt(3900);

    TEST_ASSERT_EQUAL(0, maintenanceBootRequests);
    TEST_ASSERT_EQUAL(0, wifiStopCalls);
}

void test_long_press_blocked_during_active_alert() {
    parser.setAlerts({makeAlert()});
    touch.queueTouch(20, 20);
    touch.queueTouch(20, 20);  // held through the threshold

    processAt(0);
    processAt(4000);

    // An accidental 4 s hold mid-alert must never reboot the device.
    TEST_ASSERT_EQUAL(0, maintenanceBootRequests);
    TEST_ASSERT_EQUAL(0, wifiStopCalls);
}

void test_long_press_after_release_restarts_hold_timer() {
    touch.queueTouch(20, 20);
    touch.queueNoTouch();      // release resets the hold timer
    touch.queueTouch(20, 20);  // new hold starts at 300
    touch.queueTouch(20, 20);  // held

    processAt(0);
    processAt(200);
    processAt(300);
    processAt(4200);  // 3900 ms into the second hold: below threshold

    TEST_ASSERT_EQUAL(0, maintenanceBootRequests);

    touch.queueTouch(20, 20);  // still held
    processAt(4400);  // 4100 ms into the second hold: fires

    TEST_ASSERT_EQUAL(1, maintenanceBootRequests);
    TEST_ASSERT_EQUAL(0, wifiStopCalls);
}

void test_touch_polling_is_throttled_between_cadence_ticks() {
    processAt(100);
    processAt(101);
    processAt(124);

    TEST_ASSERT_EQUAL(1, touch.getTouchPointCalls);

    processAt(125);

    TEST_ASSERT_EQUAL(2, touch.getTouchPointCalls);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_alert_tap_toggles_mute_immediately);
    RUN_TEST(test_idle_triple_tap_cycles_slot_and_pushes_when_connected);
    RUN_TEST(test_idle_profile_cycle_resets_after_tap_window_expires);
    RUN_TEST(test_long_press_requests_maintenance_boot_instead_of_starting_wifi);
    RUN_TEST(test_long_press_stops_wifi_if_already_active);
    RUN_TEST(test_long_press_below_threshold_does_not_fire);
    RUN_TEST(test_long_press_blocked_during_active_alert);
    RUN_TEST(test_long_press_after_release_restarts_hold_timer);
    RUN_TEST(test_touch_polling_is_throttled_between_cadence_ticks);
    return UNITY_END();
}
