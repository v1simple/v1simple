/**
 * CAR_MODE_PWR_SHORT integration tests.
 *
 * Verifies that compile-time safety gates work correctly in the car-install
 * build, where the device has no 18650, no TCA9554 latch, and the PWR_BUTTON
 * GPIO is soldered LOW on V_IN.
 *
 * These tests ONLY compile with -DCAR_MODE_PWR_SHORT=1 (native_car env).
 * They are excluded from the regular native env via test_ignore in platformio.ini.
 */
#ifndef CAR_MODE_PWR_SHORT
#error "This test file must be built with -DCAR_MODE_PWR_SHORT=1 (env: native_car)"
#endif

#include <unity.h>

#include <filesystem>
#include <fstream>
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

#include "../../src/modules/power/power_module.cpp"

// Also pull in the pure button helper so we can exercise it directly.
#include "../../src/battery_manager_button.cpp"

namespace {

BatteryManager battery;
V1Display display;
SettingsManager testSettings;
PowerModule module;

void setTime(unsigned long nowMs) {
    mockMillis = nowMs;
    mockMicros = nowMs * 1000;
}

void advanceTime(unsigned long deltaMs) {
    setTime(mockMillis + deltaMs);
}

std::string readProjectFile(const char* relativePath) {
    const std::filesystem::path path = std::filesystem::path(PROJECT_DIR) / relativePath;
    std::ifstream in(path);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
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
    module = PowerModule{};
    module.begin(&battery, &display, &testSettings);
}

void tearDown() {}

// ─── processPowerButton() is skipped in process() ─────────────────────────

void test_car_mode_long_press_does_not_trigger_shutdown() {
    // Even if the mock battery reports that the button was held long enough,
    // process() should not call powerOff() because the entire button-check
    // block is compiled away.
    battery.processPowerButtonResult = true;  // would fire in normal mode

    module.process(1000);

    TEST_ASSERT_FALSE(battery.powerOffCalled);
    TEST_ASSERT_EQUAL(0, battery.processPowerButtonCalls);
    TEST_ASSERT_EQUAL(0, display.showShutdownCalls);
}

void test_car_mode_process_does_not_call_processPowerButton() {
    // process() must not reach processPowerButton() at all.
    module.process(1000);
    module.process(2000);
    module.process(3000);

    TEST_ASSERT_EQUAL(0, battery.processPowerButtonCalls);
    // battery.update() is NOT gated — it must still run.
    TEST_ASSERT_EQUAL(3, battery.updateCalls);
}

// ─── Critical-battery block is skipped ─────────────────────────────────────

void test_car_mode_critical_battery_does_not_show_warning() {
    battery.setCritical(true);

    module.process(1000);

    TEST_ASSERT_EQUAL(0, display.showLowBatteryCalls);
    TEST_ASSERT_FALSE(battery.powerOffCalled);
}

void test_car_mode_critical_battery_never_triggers_shutdown() {
    battery.setCritical(true);

    module.process(1000);
    module.process(6001);  // past the 5-second grace period

    TEST_ASSERT_EQUAL(0, display.showShutdownCalls);
    TEST_ASSERT_FALSE(battery.powerOffCalled);
}

// ─── powerOff() noop ───────────────────────────────────────────────────────

// Note: the BatteryManager mock's powerOff() does NOT contain the
// CAR_MODE_PWR_SHORT guard — that guard lives in production battery_manager.cpp
// which is not compiled in native tests.  The real guard is exercised at
// firmware build time (env:esp32-s3-car-install) where the compiler will
// confirm that powerOff() returns immediately without calling TCA9554 or
// entering deep sleep.  The tests below verify behaviour at the power-module
// boundary (i.e. that the gates in power_module.cpp prevent powerOff() from
// being reached at all in the first place).

// ─── Auto-power-off still works (not gated) ────────────────────────────────

void test_car_mode_auto_power_off_still_fires_on_v1_disconnect() {
    // Auto-power-off is architecturally correct for car installs too:
    // if the V1 disconnects and the car powers off, we want the display
    // to eventually shut down.  The powerOff() noop ensures no hardware
    // damage — the firmware just logs and continues.
    testSettings.settings.autoPowerOffMinutes = 1;
    module.onV1DataReceived();
    setTime(1000);
    module.onV1ConnectionChange(false);

    advanceTime(30000);
    module.process(mockMillis);
    TEST_ASSERT_FALSE(battery.powerOffCalled);

    advanceTime(30001);
    module.process(mockMillis);

    // Auto-power-off calls batteryManager.powerOff() — the mock records it.
    // In production, the CAR_MODE_PWR_SHORT guard makes that a noop.
    TEST_ASSERT_EQUAL(1, display.showShutdownCalls);
    TEST_ASSERT_TRUE(battery.powerOffCalled);
}

// ─── Pure button logic still works (the state machine is unchanged) ────────

void test_pure_button_logic_still_triggers_at_2000ms() {
    // processPowerButtonState() is not gated — the state machine itself is
    // correct regardless of build flavor.  Only the call-site in process()
    // is removed for car installs.
    using State = BatteryManager::PwrButtonState;
    State s;
    processPowerButtonState(true, 0, s);
    TEST_ASSERT_TRUE(processPowerButtonState(true, 2000, s));
}

void test_car_mode_maintenance_entry_uses_boot_button_not_power_button() {
    const std::string runtimeWiringSource = readProjectFile("src/main_runtime_wiring.cpp");
    const std::string powerSource = readProjectFile("src/modules/power/power_module.cpp");

    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          runtimeWiringSource.find("static void requestMaintenanceBootRestart()"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          runtimeWiringSource.find(".requestMaintenanceBoot = [](void* /*ctx*/) { requestMaintenanceBootRestart(); },"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          powerSource.find("#ifndef CAR_MODE_PWR_SHORT"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          powerSource.find("battery_->processPowerButton()"));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_car_mode_long_press_does_not_trigger_shutdown);
    RUN_TEST(test_car_mode_process_does_not_call_processPowerButton);
    RUN_TEST(test_car_mode_critical_battery_does_not_show_warning);
    RUN_TEST(test_car_mode_critical_battery_never_triggers_shutdown);
    RUN_TEST(test_car_mode_auto_power_off_still_fires_on_v1_disconnect);
    RUN_TEST(test_pure_button_logic_still_triggers_at_2000ms);
    RUN_TEST(test_car_mode_maintenance_entry_uses_boot_button_not_power_button);
    return UNITY_END();
}
