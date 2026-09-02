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
    QueueResult queueSlotPush(int) { return QueueResult::QUEUED; }
};

class AlertPersistenceModule {
  public:
    void clearPersistence() {}
};

#include "../../src/modules/quiet/quiet_coordinator_module.cpp"
#include "../../src/modules/touch/tap_gesture_module.cpp"

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
int maintenanceBootRequests = 0;

bool wifiInactive(void*) { return false; }
void requestMaintenanceBoot(void*) { ++maintenanceBootRequests; }

void pollTouch(unsigned long nowMs, bool active) {
    std::vector<uint8_t> data(32, 0);
    data[1] = active ? 1 : 0;
    data[3] = data[5] = 10;
    Wire.queueRequestFrom(data.size(), data);
    mockMillis = nowMs;
    tap.process(nowMs);
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
    maintenanceBootRequests = 0;
    TapGestureModule::WifiCallbacks wifiCallbacks{};
    wifiCallbacks.isWifiActive = wifiInactive;
    wifiCallbacks.requestMaintenanceBoot = requestMaintenanceBoot;
    tap.begin(&touch, &settings, &display, &ble, &parser, &autoPush, &persistence, &displayMode, &quiet,
              wifiCallbacks);
}

void test_alert_clear_drops_stale_mute_and_does_not_starve_long_press() {
    ble.nextMuteSendResult = SendResult::NOT_YET;
    pollTouch(200, true);
    TEST_ASSERT_EQUAL_INT(1, ble.setMuteCalls);

    parser.setAlerts({});
    pollTouch(225, false);
    TEST_ASSERT_EQUAL_INT(1, ble.setMuteCalls);

    pollTouch(450, true); // Allow the real reader's tap/release debounce.
    pollTouch(4450, true);

    TEST_ASSERT_EQUAL_INT(1, maintenanceBootRequests);
    TEST_ASSERT_EQUAL_INT(1, ble.setMuteCalls);
}

void tearDown() {}

void test_active_alert_tap_retries_one_transient_mute_without_resend_after_success() {
    ble.nextMuteSendResult = SendResult::NOT_YET;
    pollTouch(200, true);
    TEST_ASSERT_EQUAL_INT(1, ble.setMuteCalls);
    TEST_ASSERT_TRUE(ble.lastMuteValue);

    tap.process(210);
    TEST_ASSERT_EQUAL_INT(1, ble.setMuteCalls);
    pollTouch(225, false);
    TEST_ASSERT_EQUAL_INT(2, ble.setMuteCalls);
    TEST_ASSERT_TRUE(ble.lastMuteValue);

    pollTouch(250, false);
    TEST_ASSERT_EQUAL_INT(2, ble.setMuteCalls);
}

void test_failed_touch_reads_cancel_hold_without_turning_recovery_into_a_tap() {
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
            mockMillis = nowMs;
            tap.process(nowMs);
            TEST_ASSERT_FALSE(touch.isTouchActive());
        }
        TEST_ASSERT_GREATER_THAN_INT(0, Wire.endCalls); // Includes recovery backoff.
        TEST_ASSERT_EQUAL_INT(0, maintenanceBootRequests);

        Wire.resetMock(); // Discard fault responses queued during backoff.
        parser.setAlerts({AlertData::create(BAND_KA, DIR_FRONT, 5, 0, 34700, true, true)});
        pollTouch(5500, true);
        TEST_ASSERT_EQUAL_INT(0, ble.setMuteCalls);

        parser.setAlerts({});
        pollTouch(9500, true);
        TEST_ASSERT_EQUAL_INT(0, maintenanceBootRequests);

        pollTouch(9525, false);
        pollTouch(9700, true);
        pollTouch(13700, true);
        TEST_ASSERT_EQUAL_INT(1, maintenanceBootRequests);
    }
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_active_alert_tap_retries_one_transient_mute_without_resend_after_success);
    RUN_TEST(test_alert_clear_drops_stale_mute_and_does_not_starve_long_press);
    RUN_TEST(test_failed_touch_reads_cancel_hold_without_turning_recovery_into_a_tap);
    return UNITY_END();
}
