#include <unity.h>

#include "../mocks/Arduino.h"
#include "../mocks/battery_manager.h"
#include "../mocks/display.h"
#include "../mocks/settings.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#include "../../src/modules/power/power_module.cpp"

namespace {

BatteryManager battery;
V1Display display;
SettingsManager testSettings;
PowerModule module;
struct ShutdownPrepState {
    int calls = 0;
    int showShutdownCallsAtPrep = -1;
    int powerOffCallsAtPrep = -1;
};

ShutdownPrepState shutdownPrepState;

void recordShutdownPreparation(void* context) {
    auto* state = static_cast<ShutdownPrepState*>(context);
    if (!state) {
        return;
    }

    state->calls++;
    state->showShutdownCallsAtPrep = display.showShutdownCalls;
    state->powerOffCallsAtPrep = battery.powerOffCalls;
}

void setTime(unsigned long nowMs) {
    mockMillis = nowMs;
    mockMicros = nowMs * 1000;
}

void advanceTime(unsigned long deltaMs) {
    setTime(mockMillis + deltaMs);
}

}  // namespace

void setUp() {
    setTime(0);
    battery.reset();
    battery.setOnBattery(true);
    battery.setHasBattery(true);
    battery.setBatteryPercent(60);
    battery.setVoltage(3.8f);

    display.reset();

    testSettings = SettingsManager{};
    testSettings.settings.autoPowerOffMinutes = 10;
    shutdownPrepState = ShutdownPrepState{};

    module = PowerModule{};
    module.begin(&battery, &display, &testSettings);
}

void tearDown() {}

void test_critical_battery_shows_warning_before_shutdown() {
    battery.setCritical(true);

    module.process(1000);

    TEST_ASSERT_EQUAL(1, display.showLowBatteryCalls);
    TEST_ASSERT_TRUE(module.lowBatteryWarningShownForTest());
    TEST_ASSERT_FALSE(battery.powerOffCalled);
}

void test_perform_shutdown_request_delegates_to_battery_power_off() {
    module.performShutdownRequestForTest();

    TEST_ASSERT_EQUAL(1, display.showShutdownCalls);
#ifndef CAR_MODE_PWR_SHORT
    TEST_ASSERT_EQUAL(1, display.clearCalls);
#endif
    TEST_ASSERT_TRUE(battery.powerOffCalled);
    TEST_ASSERT_EQUAL(1, battery.powerOffCalls);
}

void test_shutdown_leaves_panel_frame_black_before_power_handoff() {
    module.performShutdownRequestForTest();

    TEST_ASSERT_EQUAL(1, display.showShutdownCalls);
#ifndef CAR_MODE_PWR_SHORT
    TEST_ASSERT_EQUAL(1, display.clearCalls);
#else
    TEST_ASSERT_EQUAL(0, display.clearCalls);
#endif
    TEST_ASSERT_EQUAL(1, battery.powerOffCalls);
}

void test_failed_shutdown_restores_visible_disconnected_screen() {
    testSettings.settings.brightness = 173;
    battery.powerOffResult = false;

    module.performShutdownRequestForTest();

    TEST_ASSERT_EQUAL(1, battery.powerOffCalls);
    TEST_ASSERT_EQUAL(1, display.showDisconnectedCalls);
    TEST_ASSERT_EQUAL(1, display.flushCalls);
    TEST_ASSERT_EQUAL(1, display.setBrightnessCalls);
    TEST_ASSERT_EQUAL_UINT8(173, display.lastBrightness);
    TEST_ASSERT_TRUE(display.showDisconnectedSequence > 0);
    TEST_ASSERT_TRUE(display.showDisconnectedSequence < display.flushSequence);
    TEST_ASSERT_TRUE(display.flushSequence < display.setBrightnessSequence);
}

void test_set_shutdown_preparation_callback_runs_before_shutdown_tail() {
    module.setShutdownPreparationCallback(recordShutdownPreparation, &shutdownPrepState);

    module.performShutdownRequestForTest();

    TEST_ASSERT_EQUAL(1, shutdownPrepState.calls);
    TEST_ASSERT_EQUAL(0, shutdownPrepState.showShutdownCallsAtPrep);
    TEST_ASSERT_EQUAL(0, shutdownPrepState.powerOffCallsAtPrep);
    TEST_ASSERT_EQUAL(1, display.showShutdownCalls);
    TEST_ASSERT_EQUAL(1, battery.powerOffCalls);
}

void test_null_shutdown_preparation_callback_is_tolerated() {
    module.setShutdownPreparationCallback(nullptr, nullptr);

    module.performShutdownRequestForTest();

    TEST_ASSERT_EQUAL(1, display.showShutdownCalls);
    TEST_ASSERT_EQUAL(1, battery.powerOffCalls);
}

void test_power_button_shutdown_request_routes_through_power_module() {
    battery.processPowerButtonResult = true;
    battery.setCritical(true);

    module.process(1000);

    TEST_ASSERT_EQUAL(1, display.showShutdownCalls);
    TEST_ASSERT_TRUE(battery.powerOffCalled);
    TEST_ASSERT_EQUAL(1, battery.powerOffCalls);
    TEST_ASSERT_EQUAL(0, display.showLowBatteryCalls);
}

void test_critical_battery_shutdown_occurs_after_grace_period() {
    battery.setCritical(true);

    module.process(1000);
    module.process(6001);

    TEST_ASSERT_EQUAL(1, display.showShutdownCalls);
    TEST_ASSERT_TRUE(battery.powerOffCalled);
    TEST_ASSERT_EQUAL(1, battery.powerOffCalls);
}

void test_critical_battery_recovery_clears_warning_without_shutdown() {
    battery.setCritical(true);
    module.process(1000);

    battery.setCritical(false);
    module.process(3000);
    module.process(7000);

    TEST_ASSERT_FALSE(module.lowBatteryWarningShownForTest());
    TEST_ASSERT_FALSE(battery.powerOffCalled);
}

void test_usb_power_skips_critical_battery_shutdown_path() {
    battery.setOnBattery(false);
    battery.setCritical(true);

    module.process(1000);
    module.process(10000);

    TEST_ASSERT_EQUAL(0, display.showLowBatteryCalls);
    TEST_ASSERT_FALSE(battery.powerOffCalled);
}

void test_v1_data_arms_auto_power_off() {
    TEST_ASSERT_FALSE(module.autoPowerOffArmedForTest());

    module.onV1DataReceived();

    TEST_ASSERT_TRUE(module.autoPowerOffArmedForTest());
}

void test_disconnect_starts_auto_power_timer_when_armed() {
    module.onV1DataReceived();
    setTime(5000);

    module.onV1ConnectionChange(false);

    TEST_ASSERT_EQUAL(5000UL, module.autoPowerOffTimerStartForTest());
}

void test_alp_activity_arms_auto_power_off() {
    TEST_ASSERT_FALSE(module.autoPowerOffArmedForTest());

    module.onAlpSignalChange(true);

    TEST_ASSERT_TRUE(module.autoPowerOffArmedForTest());
}

void test_alp_signal_loss_starts_auto_power_timer_when_armed() {
    module.onAlpSignalChange(true);
    setTime(7000);

    module.onAlpSignalChange(false);

    TEST_ASSERT_EQUAL(7000UL, module.autoPowerOffTimerStartForTest());
}

void test_disconnect_does_not_start_timer_when_not_armed_or_disabled() {
    module.onV1ConnectionChange(false);
    TEST_ASSERT_EQUAL(0UL, module.autoPowerOffTimerStartForTest());

    module.onV1DataReceived();
    testSettings.settings.autoPowerOffMinutes = 0;
    module.onV1ConnectionChange(false);

    TEST_ASSERT_EQUAL(0UL, module.autoPowerOffTimerStartForTest());
}

void test_reconnect_cancels_running_auto_power_timer() {
    module.onV1DataReceived();
    setTime(1000);
    module.onV1ConnectionChange(false);
    TEST_ASSERT_EQUAL(1000UL, module.autoPowerOffTimerStartForTest());

    module.onV1ConnectionChange(true);

    TEST_ASSERT_EQUAL(0UL, module.autoPowerOffTimerStartForTest());
}

void test_alp_activity_cancels_running_auto_power_timer() {
    module.onAlpSignalChange(true);
    setTime(1000);
    module.onAlpSignalChange(false);
    TEST_ASSERT_EQUAL(1000UL, module.autoPowerOffTimerStartForTest());

    module.onAlpSignalChange(true);

    TEST_ASSERT_EQUAL(0UL, module.autoPowerOffTimerStartForTest());
}

void test_v1_disconnect_does_not_start_timer_while_alp_is_active() {
    module.onV1DataReceived();
    module.onAlpSignalChange(true);
    setTime(2000);

    module.onV1ConnectionChange(false);

    TEST_ASSERT_EQUAL(0UL, module.autoPowerOffTimerStartForTest());
}

void test_alp_signal_loss_starts_timer_after_v1_disconnect() {
    module.onV1DataReceived();
    module.onV1ConnectionChange(true);
    module.onAlpSignalChange(true);
    setTime(1000);
    module.onV1ConnectionChange(false);
    TEST_ASSERT_EQUAL(0UL, module.autoPowerOffTimerStartForTest());

    setTime(4000);
    module.onAlpSignalChange(false);

    TEST_ASSERT_EQUAL(4000UL, module.autoPowerOffTimerStartForTest());
}

void test_auto_power_off_shuts_down_after_timeout() {
    testSettings.settings.autoPowerOffMinutes = 1;
    module.onV1DataReceived();
    setTime(1000);
    module.onV1ConnectionChange(false);

    advanceTime(30000);
    module.process(mockMillis);
    TEST_ASSERT_FALSE(battery.powerOffCalled);

    advanceTime(30001);
    module.process(mockMillis);

    TEST_ASSERT_EQUAL(1, display.showShutdownCalls);
    TEST_ASSERT_TRUE(battery.powerOffCalled);
    TEST_ASSERT_EQUAL(1, battery.powerOffCalls);
}

void test_process_updates_battery_every_call() {
    module.process(1000);
    module.process(2000);
    module.process(3000);

    TEST_ASSERT_EQUAL(3, battery.updateCalls);
    TEST_ASSERT_EQUAL(3, battery.processPowerButtonCalls);
}

void test_critical_battery_still_wins_while_auto_power_timer_is_running() {
    testSettings.settings.autoPowerOffMinutes = 10;
    module.onV1DataReceived();
    setTime(1000);
    module.onV1ConnectionChange(false);

    battery.setCritical(true);
    advanceTime(1000);
    module.process(mockMillis);
    TEST_ASSERT_EQUAL(1, display.showLowBatteryCalls);
    TEST_ASSERT_FALSE(battery.powerOffCalled);

    advanceTime(5001);
    module.process(mockMillis);

    TEST_ASSERT_EQUAL(1, display.showShutdownCalls);
    TEST_ASSERT_TRUE(battery.powerOffCalled);
}

int main() {
    UNITY_BEGIN();
#ifndef CAR_MODE_PWR_SHORT
    // These paths are compile-time disabled in car-install builds.
    // Equivalent coverage lives in test_car_mode_pwr_short.
    RUN_TEST(test_critical_battery_shows_warning_before_shutdown);
#endif
    RUN_TEST(test_perform_shutdown_request_delegates_to_battery_power_off);
    RUN_TEST(test_shutdown_leaves_panel_frame_black_before_power_handoff);
    RUN_TEST(test_failed_shutdown_restores_visible_disconnected_screen);
    RUN_TEST(test_set_shutdown_preparation_callback_runs_before_shutdown_tail);
    RUN_TEST(test_null_shutdown_preparation_callback_is_tolerated);
#ifndef CAR_MODE_PWR_SHORT
    RUN_TEST(test_power_button_shutdown_request_routes_through_power_module);
    RUN_TEST(test_critical_battery_shutdown_occurs_after_grace_period);
#endif
    RUN_TEST(test_critical_battery_recovery_clears_warning_without_shutdown);
    RUN_TEST(test_usb_power_skips_critical_battery_shutdown_path);
    RUN_TEST(test_v1_data_arms_auto_power_off);
    RUN_TEST(test_disconnect_starts_auto_power_timer_when_armed);
    RUN_TEST(test_alp_activity_arms_auto_power_off);
    RUN_TEST(test_alp_signal_loss_starts_auto_power_timer_when_armed);
    RUN_TEST(test_disconnect_does_not_start_timer_when_not_armed_or_disabled);
    RUN_TEST(test_reconnect_cancels_running_auto_power_timer);
    RUN_TEST(test_alp_activity_cancels_running_auto_power_timer);
    RUN_TEST(test_v1_disconnect_does_not_start_timer_while_alp_is_active);
    RUN_TEST(test_alp_signal_loss_starts_timer_after_v1_disconnect);
    RUN_TEST(test_auto_power_off_shuts_down_after_timeout);
#ifndef CAR_MODE_PWR_SHORT
    RUN_TEST(test_process_updates_battery_every_call);
    RUN_TEST(test_critical_battery_still_wins_while_auto_power_timer_is_running);
#endif
    return UNITY_END();
}
