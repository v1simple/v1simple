#include <unity.h>

#include "../mocks/Arduino.h"
#include "../mocks/ble_client.h"
#include "../mocks/display.h"
#include "../mocks/packet_parser.h"
#include "../mocks/settings.h"
#include "../mocks/touch_handler.h"
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
}

void setUp() {
    touch.reset();
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
    touch.queueTouch(10, 10);
    tap.process(200);
    TEST_ASSERT_EQUAL_INT(1, ble.setMuteCalls);

    parser.setAlerts({});
    touch.queueNoTouch();
    tap.process(225);
    TEST_ASSERT_EQUAL_INT(1, ble.setMuteCalls);

    touch.queueTouch(10, 10);
    tap.process(250);
    touch.queueTouch(10, 10); // same held touch: level remains active, no new edge
    tap.process(4250);

    TEST_ASSERT_EQUAL_INT(1, maintenanceBootRequests);
    TEST_ASSERT_EQUAL_INT(1, ble.setMuteCalls);
}

void tearDown() {}

void test_active_alert_tap_retries_one_transient_mute_without_resend_after_success() {
    ble.nextMuteSendResult = SendResult::NOT_YET;
    touch.queueTouch(10, 10);
    tap.process(200);
    TEST_ASSERT_EQUAL_INT(1, ble.setMuteCalls);
    TEST_ASSERT_TRUE(ble.lastMuteValue);

    tap.process(210);
    TEST_ASSERT_EQUAL_INT(1, ble.setMuteCalls);
    tap.process(225);
    TEST_ASSERT_EQUAL_INT(2, ble.setMuteCalls);
    TEST_ASSERT_TRUE(ble.lastMuteValue);

    tap.process(250);
    TEST_ASSERT_EQUAL_INT(2, ble.setMuteCalls);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_active_alert_tap_retries_one_transient_mute_without_resend_after_success);
    RUN_TEST(test_alert_clear_drops_stale_mute_and_does_not_starve_long_press);
    return UNITY_END();
}
