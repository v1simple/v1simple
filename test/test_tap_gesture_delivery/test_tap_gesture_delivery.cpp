#include <unity.h>

#include "../mocks/Arduino.h"
#include "../../src/touch_handler.cpp"
#include "../mocks/ble_client.h"
#include "../mocks/display.h"
#include "../mocks/packet_parser.h"
#include "../mocks/settings.h"
#include "../../include/display_mode.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

class AutoPushModule {
  public:
    enum class QueueResult : uint8_t { QUEUED };
    bool isActive() const { return false; }
    QueueResult queueSlotPush(int) { return QueueResult::QUEUED; }
};

class AlertPersistenceModule {
  public:
    void clearPersistence() {}
};

#include "../../src/modules/quiet/quiet_coordinator_module.cpp"
#include "../../src/modules/touch/tap_gesture_module.cpp"
#include "../../src/modules/touch/touch_ui_module.cpp"

void audio_set_volume(uint8_t) {}
void play_test_voice() {}

namespace {
TouchHandler touch;
SettingsManager settings;
V1Display display;
V1BLEClient ble;
PacketParser parser;
AutoPushModule autoPush;
AlertPersistenceModule persistence;
DisplayMode displayMode = DisplayMode::IDLE;
QuietCoordinatorModule quiet;
TapGestureModule tap;
TouchUiModule touchUi;
int maintenanceBootRequests = 0;

bool wifiInactive(void*) { return false; }
void requestMaintenanceBoot(void*) { ++maintenanceBootRequests; }

void processInput(unsigned long nowMs, bool bootPressed = false) {
    mockMillis = nowMs;
    // Match normal runtime ownership: BOOT/settings first, then screen taps.
    if (!touchUi.process(nowMs, bootPressed)) {
        tap.process(nowMs);
    }
}

void pollTouch(unsigned long nowMs, bool active, uint16_t coordinate = 10) {
    std::vector<uint8_t> data(32, 0);
    data[1] = active ? 1 : 0;
    data[2] = data[4] = coordinate >> 8;
    data[3] = data[5] = coordinate;
    Wire.queueRequestFrom(data.size(), data);
    processInput(nowMs);
}
}

void setUp() {
    mockMillis = 0;
    Wire.resetMock();
    touch = TouchHandler{};
    TEST_ASSERT_TRUE(touch.begin());
    settings = SettingsManager{};
    display = V1Display{};
    ble.reset();
    parser.reset();
    parser.setAlerts({AlertData::create(BAND_KA, DIR_FRONT, 5, 0, 34700, true, true)});
    parser.setMuted(false);
    quiet = QuietCoordinatorModule{};
    quiet.begin(&ble, &parser);
    tap = TapGestureModule{};
    touchUi = TouchUiModule{};
    maintenanceBootRequests = 0;
    TouchUiModule::Callbacks callbacks{};
    callbacks.isWifiSetupActive = wifiInactive;
    callbacks.requestMaintenanceBoot = requestMaintenanceBoot;
    touchUi.begin(&display, &touch, &settings, callbacks);
    tap.begin(&touch, &settings, &display, &ble, &parser, &autoPush, &persistence, &displayMode, &quiet);
}

void test_alert_clear_drops_stale_mute_retry() {
    ble.nextMuteSendResult = SendResult::NOT_YET;
    pollTouch(200, true);
    TEST_ASSERT_EQUAL_INT(1, ble.setMuteCalls);

    parser.setAlerts({});
    pollTouch(225, false);
    TEST_ASSERT_EQUAL_INT(1, ble.setMuteCalls);

    pollTouch(450, true); // Allow the real reader's tap/release debounce.
    TEST_ASSERT_EQUAL_INT(1, ble.setMuteCalls);
}

void tearDown() {}

void test_active_alert_tap_retries_one_transient_mute_without_resend_after_success() {
    ble.nextMuteSendResult = SendResult::NOT_YET;
    pollTouch(200, true);
    TEST_ASSERT_EQUAL_INT(1, ble.setMuteCalls);
    TEST_ASSERT_TRUE(ble.lastMuteValue);

    processInput(210);
    TEST_ASSERT_EQUAL_INT(1, ble.setMuteCalls);
    pollTouch(225, false);
    TEST_ASSERT_EQUAL_INT(2, ble.setMuteCalls);
    TEST_ASSERT_TRUE(ble.lastMuteValue);

    pollTouch(250, false);
    TEST_ASSERT_EQUAL_INT(2, ble.setMuteCalls);
}

void test_failed_touch_reads_invalidate_level_without_turning_recovery_into_a_tap() {
    for (bool shortRead : {false, true}) {
        setUp();
        parser.setAlerts({});
        pollTouch(1000, true);

        for (unsigned long nowMs = 1025; nowMs <= 5100; nowMs += 25) {
            if (shortRead) {
                Wire.queueRequestFrom(0, {});
            } else {
                Wire.queueEndTransmission(2);
            }
            processInput(nowMs);
            TEST_ASSERT_FALSE(touch.isTouchActive());
        }
        TEST_ASSERT_GREATER_THAN_INT(0, Wire.endCalls); // Includes recovery backoff.
        TEST_ASSERT_EQUAL_INT(0, maintenanceBootRequests);

        Wire.resetMock(); // Discard fault responses queued during backoff.
        parser.setAlerts({AlertData::create(BAND_KA, DIR_FRONT, 5, 0, 34700, true, true)});
        pollTouch(5500, true);
        TEST_ASSERT_EQUAL_INT(0, ble.setMuteCalls);

        pollTouch(9500, true);
        TEST_ASSERT_EQUAL_INT(0, ble.setMuteCalls);

        pollTouch(9525, false);
        pollTouch(9700, true);
        TEST_ASSERT_EQUAL_INT(1, ble.setMuteCalls);
        pollTouch(13700, true);
        TEST_ASSERT_EQUAL_INT(1, ble.setMuteCalls);
        TEST_ASSERT_EQUAL_INT(0, maintenanceBootRequests);
    }
}

void test_screen_holds_cannot_request_maintenance_but_boot_hold_can() {
    for (uint16_t coordinate : {10, 514}) {
        setUp();
        parser.setAlerts({});
        for (unsigned long nowMs = 1000; nowMs <= 5500; nowMs += 25) {
            pollTouch(nowMs, true, coordinate);
            TEST_ASSERT_EQUAL(coordinate == 10, touch.isTouchActive());
            TEST_ASSERT_EQUAL_INT(0, maintenanceBootRequests);
        }
        TEST_ASSERT_EQUAL_UINT8(0, settings.get().activeSlot);

        // Exercise the same live callback through its sole authorized input.
        processInput(6000, true);
        processInput(10000, true);
        TEST_ASSERT_EQUAL_INT(0, maintenanceBootRequests);
        processInput(10025, false);
        TEST_ASSERT_EQUAL_INT(1, maintenanceBootRequests);
    }
}

void test_three_screen_taps_still_cycle_profile_once() {
    parser.setAlerts({});
    pollTouch(1000, true);
    pollTouch(1050, false);
    pollTouch(1250, true);
    TEST_ASSERT_EQUAL_UINT8(0, settings.get().activeSlot);
    pollTouch(1300, false);
    pollTouch(1500, true);
    TEST_ASSERT_EQUAL_UINT8(1, settings.get().activeSlot);
    TEST_ASSERT_EQUAL_INT(1, settings.saveDeferredBackupCalls);
    TEST_ASSERT_EQUAL_INT(1, display.drawProfileIndicatorCalls);
    pollTouch(5500, true);
    TEST_ASSERT_EQUAL_UINT8(1, settings.get().activeSlot);
    TEST_ASSERT_EQUAL_INT(1, display.drawProfileIndicatorCalls);
    TEST_ASSERT_EQUAL_INT(0, maintenanceBootRequests);
}

void test_invalid_touch_coordinates_cannot_mute_a_live_alert() {
    pollTouch(1000, true, 514);
    TEST_ASSERT_FALSE(touch.isTouchActive());
    TEST_ASSERT_EQUAL_INT(0, ble.setMuteCalls);
    pollTouch(1250, true, 10);
    TEST_ASSERT_TRUE(touch.isTouchActive());
    TEST_ASSERT_EQUAL_INT(1, ble.setMuteCalls);
    TEST_ASSERT_TRUE(ble.lastMuteValue);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_invalid_touch_coordinates_cannot_mute_a_live_alert);
    RUN_TEST(test_active_alert_tap_retries_one_transient_mute_without_resend_after_success);
    RUN_TEST(test_alert_clear_drops_stale_mute_retry);
    RUN_TEST(test_failed_touch_reads_invalidate_level_without_turning_recovery_into_a_tap);
    RUN_TEST(test_screen_holds_cannot_request_maintenance_but_boot_hold_can);
    RUN_TEST(test_three_screen_taps_still_cycle_profile_once);
    return UNITY_END();
}
