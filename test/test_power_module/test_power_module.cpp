#include <unity.h>

#include <string>

#include "../mocks/Arduino.h"
#include "../mocks/battery_manager.h"
#include "../mocks/display.h"
#include "../mocks/settings.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#include "../../include/battery_math.h"
#include "../../src/modules/power/power_module.cpp"

namespace {

BatteryManager battery;
V1Display display;
SettingsManager settings;
PowerModule power;

void setTime(unsigned long nowMs) {
    mockMillis = nowMs;
    mockMicros = nowMs * 1000UL;
}

struct ProbePowerLifecycle final : PowerLifecycle {
    int* preparationCalls = nullptr;
    int* abortCalls = nullptr;

    void prepareForShutdown() override {
        if (preparationCalls) {
            ++*preparationCalls;
        }
    }
    void resumeAfterAbortedShutdown() override {
        if (abortCalls) {
            ++*abortCalls;
        }
    }
};

} // namespace

void setUp() {
    setTime(0);
    battery.reset();
    battery.setOnBattery(true);
    battery.setHasBattery(true);
    battery.setBatteryPercent(60);
    display.reset();
    settings = SettingsManager{};
    settings.settings.autoPowerOffMinutes = 1;
    power = PowerModule{};
    power.begin(&battery, &display, &settings);
}

void tearDown() {}

void test_critical_protection_keeps_zero_invalid_but_accepts_deep_discharge() {
    TEST_ASSERT_TRUE(battery_math::criticalProtectionRequired(true, 3249));
    TEST_ASSERT_TRUE(battery_math::criticalProtectionRequired(true, 3200));
    TEST_ASSERT_TRUE(battery_math::criticalProtectionRequired(true, 3199));
    TEST_ASSERT_TRUE(battery_math::criticalProtectionRequired(true, 1));
    TEST_ASSERT_FALSE(battery_math::criticalProtectionRequired(true, 0));
    TEST_ASSERT_FALSE(battery_math::criticalProtectionRequired(false, 3199));
    TEST_ASSERT_FALSE(battery_math::criticalProtectionRequired(false, 1));
}

void test_critical_warning_does_not_depend_on_icon_presence_floor() {
    battery.setHasBattery(false);
    battery.setCritical(true);

    power.process(100);

    TEST_ASSERT_TRUE(power.ownsDisplayPresentation());
    TEST_ASSERT_EQUAL(1, display.showLowBatteryCalls);
    TEST_ASSERT_FALSE(power.consumeDisplayRestoreRequest());
}

void test_usb_transition_releases_warning_and_requests_one_authoritative_restore() {
    battery.setCritical(true);
    power.process(100);
    TEST_ASSERT_TRUE(power.ownsDisplayPresentation());

    battery.setOnBattery(false);
    power.process(200);

    TEST_ASSERT_FALSE(power.ownsDisplayPresentation());
    TEST_ASSERT_TRUE(power.consumeDisplayRestoreRequest());
    TEST_ASSERT_FALSE(power.consumeDisplayRestoreRequest());
}

void test_critical_shutdown_abort_releases_warning_for_authoritative_recovery() {
    int abortCalls = 0;
    ProbePowerLifecycle lifecycle;
    lifecycle.abortCalls = &abortCalls;
    power.setLifecycle(lifecycle);
    battery.setCritical(true);
    battery.powerOffResult = false;
    power.process(100);

    setTime(5101);
    power.process(5101);

    TEST_ASSERT_EQUAL(1, battery.refreshVoltageCalls);
    TEST_ASSERT_EQUAL(1, battery.powerOffCalls);
    TEST_ASSERT_EQUAL(1, abortCalls);
    TEST_ASSERT_FALSE(power.ownsDisplayPresentation());
    TEST_ASSERT_TRUE(power.consumeDisplayRestoreRequest());
    TEST_ASSERT_TRUE(power.consumeDisplayBrightnessRestoreRequest());
    TEST_ASSERT_EQUAL(0, display.setBrightnessCalls);
}

void test_critical_shutdown_is_cancelled_when_fresh_read_is_unavailable() {
    battery.setCritical(true);
    battery.setRefreshVoltageResult(false);
    power.process(100);

    power.process(5101);

    TEST_ASSERT_EQUAL(1, battery.refreshVoltageCalls);
    TEST_ASSERT_EQUAL(0, battery.powerOffCalls);
    TEST_ASSERT_FALSE(power.ownsDisplayPresentation());
    TEST_ASSERT_TRUE(power.consumeDisplayRestoreRequest());
}

void test_critical_shutdown_is_cancelled_when_fresh_read_recovers() {
    battery.setCritical(true);
    battery.setCriticalAfterRefresh(false);
    power.process(100);

    power.process(5101);

    TEST_ASSERT_EQUAL(1, battery.refreshVoltageCalls);
    TEST_ASSERT_EQUAL(0, battery.powerOffCalls);
    TEST_ASSERT_FALSE(power.ownsDisplayPresentation());
    TEST_ASSERT_TRUE(power.consumeDisplayRestoreRequest());
}

void test_critical_shutdown_reuses_successful_warning_window_read() {
    battery.setCritical(true);
    power.process(100);
    battery.setVoltageReadingAt(200);

    power.process(5101);

    TEST_ASSERT_EQUAL(0, battery.refreshVoltageCalls);
    TEST_ASSERT_EQUAL(1, battery.powerOffCalls);
}

void test_critical_shutdown_does_not_accept_pre_warning_read() {
    battery.setCritical(true);
    battery.setVoltageReadingAt(99);
    battery.setRefreshVoltageResult(false);
    power.process(100);

    power.process(5101);

    TEST_ASSERT_EQUAL(1, battery.refreshVoltageCalls);
    TEST_ASSERT_EQUAL(0, battery.powerOffCalls);
    TEST_ASSERT_FALSE(power.ownsDisplayPresentation());
}

void test_critical_shutdown_does_not_treat_trigger_sample_as_confirmation() {
    battery.setCritical(true);
    battery.setVoltageReadingAt(100);
    battery.setRefreshVoltageResult(false);
    power.process(100);

    power.process(5101);

    TEST_ASSERT_EQUAL(1, battery.refreshVoltageCalls);
    TEST_ASSERT_EQUAL(0, battery.powerOffCalls);
}

void test_auto_power_abort_retries_only_after_a_full_interval() {
    power.onV1DataReceived();
    setTime(1000);
    power.onV1ConnectionChange(false);
    TEST_ASSERT_EQUAL_UINT32(1000, power.autoPowerOffTimerStartForTest());
    battery.powerOffResult = false;

    setTime(61000);
    power.process(61000);
    TEST_ASSERT_EQUAL(1, battery.powerOffCalls);
    TEST_ASSERT_EQUAL_UINT32(61000, power.autoPowerOffTimerStartForTest());

    setTime(120999);
    power.process(120999);
    TEST_ASSERT_EQUAL(1, battery.powerOffCalls);

    setTime(121000);
    power.process(121000);
    TEST_ASSERT_EQUAL(2, battery.powerOffCalls);
    TEST_ASSERT_EQUAL_UINT32(121000, power.autoPowerOffTimerStartForTest());
}

void test_shutdown_preparation_cannot_veto_physical_poweroff() {
    int preparationCalls = 0;
    int abortCalls = 0;
    ProbePowerLifecycle lifecycle;
    lifecycle.preparationCalls = &preparationCalls;
    lifecycle.abortCalls = &abortCalls;
    power.setLifecycle(lifecycle);

    power.performShutdown();

    TEST_ASSERT_EQUAL(1, preparationCalls);
    TEST_ASSERT_EQUAL(0, abortCalls);
    TEST_ASSERT_EQUAL(1, display.showShutdownCalls);
    TEST_ASSERT_EQUAL(1, battery.powerOffCalls);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_critical_protection_keeps_zero_invalid_but_accepts_deep_discharge);
    RUN_TEST(test_critical_warning_does_not_depend_on_icon_presence_floor);
    RUN_TEST(test_usb_transition_releases_warning_and_requests_one_authoritative_restore);
    RUN_TEST(test_critical_shutdown_abort_releases_warning_for_authoritative_recovery);
    RUN_TEST(test_critical_shutdown_is_cancelled_when_fresh_read_is_unavailable);
    RUN_TEST(test_critical_shutdown_is_cancelled_when_fresh_read_recovers);
    RUN_TEST(test_critical_shutdown_reuses_successful_warning_window_read);
    RUN_TEST(test_critical_shutdown_does_not_accept_pre_warning_read);
    RUN_TEST(test_critical_shutdown_does_not_treat_trigger_sample_as_confirmation);
    RUN_TEST(test_auto_power_abort_retries_only_after_a_full_interval);
    RUN_TEST(test_shutdown_preparation_cannot_veto_physical_poweroff);
    return UNITY_END();
}
