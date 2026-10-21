// Actual BatteryManager declaration and methods. Only physical I/O is adapted.
#define digitalRead unusedDefaultDigitalRead
#define delay unusedDefaultDelay
#include "Arduino.h"
#undef digitalRead
#undef delay
#include <cassert>
#include <cstdio>
#define private public
#include "src/battery_manager.h"
#undef private
#include "audio_i2c_utils.h"
#include "battery_latch_shutdown_policy.h"
#include "poweroff_policy.h"

SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
TwoWire tca9554Wire;
SemaphoreHandle_t tca9554WireMutex = nullptr;
static void* adc1_handle = nullptr;
static bool adcAvailable = true, adcReadOk = true, externallyPowered = true;
static bool latchWriteOk = true, latchIsLow = false, pwrLow = false, bootLow = false;
static uint16_t voltageMv = 3800;
static unsigned latchWrites = 0;
struct RailCut {};
struct Slept { uint64_t mask; };
int digitalRead(int pin) { return (pin == PWR_BUTTON_GPIO ? pwrLow : bootLow) ? LOW : HIGH; }
void delay(unsigned long ms) {
    mockMillis = static_cast<uint32_t>(mockMillis + ms);
    if (latchIsLow && !externallyPowered) throw RailCut{};
}
namespace waveshare_349 {
struct Hardware { bool detectAndConfigure() { return true; } };
Hardware& hardware() { static Hardware h; return h; }
}
constexpr uint16_t kShutdownReadbackTimeoutMs = 50;
constexpr uint32_t kPowerButtonReleaseWaitMs = 1500, kBootButtonReleaseWaitMs = 250;
#ifdef LEGACY_GPIO_CLASSIFIER
constexpr battery_source_policy::Config kSourcePolicy{};
constexpr uint8_t kBootClassificationRounds = 2;
#endif
bool BatteryManager::initADC() { adc1_handle = adcAvailable ? reinterpret_cast<void*>(1) : nullptr; return adcAvailable; }
bool BatteryManager::initTCA9554() { return true; }
bool BatteryManager::latchPowerOn() { latchIsLow = false; return true; }
bool BatteryManager::readADCMillivolts(uint16_t& value) {
    if (!adc1_handle || !adcReadOk) return false;
    value = voltageMv; return true;
}
bool BatteryManager::setTCA9554PinWithBudget(uint8_t pin, bool high, TickType_t, int) {
    assert(pin == TCA9554_PWR_LATCH_PIN && !high); ++latchWrites;
    if (latchWriteOk) latchIsLow = true;
    return latchWriteOk;
}
AudioI2cResult readTca9554RegisterWithTimeout(uint8_t, uint8_t& value, TickType_t, uint16_t) {
    value = latchIsLow ? 0 : 1 << TCA9554_PWR_LATCH_PIN; return AudioI2cResult::Ok;
}
const char* audioI2cResultToString(AudioI2cResult) { return "fixture"; }
void blankPanelBacklightForSleepOrPowerOff() {}
int esp_task_wdt_reset() { return 0; }
bool waitForPinHigh(uint8_t pin, uint32_t timeoutMs) {
    if (digitalRead(pin) == HIGH) return true;
    delay(timeoutMs); return digitalRead(pin) == HIGH;
}
uint64_t wakeMaskForPlan(const poweroff_policy::WakePlan& plan) {
    return 1ULL << (plan.input == poweroff_policy::WakeInput::PWR_GPIO16 ? PWR_BUTTON_GPIO : BOOT_BUTTON_GPIO);
}
bool BatteryManager::enterDeepSleep(uint64_t mask, uint64_t, const char*) {
    if (((mask & (1ULL << PWR_BUTTON_GPIO)) && pwrLow) || ((mask & (1ULL << BOOT_BUTTON_GPIO)) && bootLow))
        return false;
    throw Slept{mask};
}
#include "src/battery_manager_button.cpp"
#include "battery_owners.inc"

static void reset() {
    mockMillis = 0; adcAvailable = adcReadOk = externallyPowered = latchWriteOk = true;
    latchIsLow = pwrLow = bootLow = false; voltageMv = 3800; latchWrites = 0;
}
static void tick(BatteryManager& battery, uint32_t time) { mockMillis = time; battery.update(); }
static void bootHeldDoesNotBecomeUsb() {
    reset(); pwrLow = true; voltageMv = 3100;
    BatteryManager battery; assert(battery.begin());
    for (uint32_t time = 100; time <= 6000; time += 100) {
        tick(battery, time);
        assert(!battery.processPowerButton());
        assert(battery.isOnBattery()); // Unknown must retain protection, even past the old 3s USB threshold.
        assert(battery.criticalProtectionRequired());
    }
    pwrLow = false; assert(!battery.processPowerButton());
    pwrLow = true; mockMillis = 7000; assert(!battery.processPowerButton());
    mockMillis = 8999; assert(!battery.processPowerButton());
    mockMillis = 9000; assert(battery.processPowerButton());
}
static void inputDoesNotChangeVoltagePresenceOrProtection() {
    for (bool external : {false, true}) {
        for (uint16_t voltage : {0, 3100, 3800}) {
            reset(); externallyPowered = external; voltageMv = voltage;
            BatteryManager battery; assert(battery.begin());
            assert(!battery.processPowerButton()); // Establish released-at-boot guard.
            const bool present = battery.hasBattery(), critical = battery.criticalProtectionRequired();
            for (uint32_t time = 100; time <= 6000; time += 100) {
                pwrLow = time >= 1000 && time < 5000;
                tick(battery, time); (void)battery.processPowerButton();
                assert(battery.hasBattery() == present);
                assert(battery.criticalProtectionRequired() == critical);
                assert(battery.getVoltageMillivolts() == voltage);
            }
        }
    }
}
static void failedAdcStillPermitsReleasedThenHeldButton() {
    reset(); adcAvailable = false;
    BatteryManager battery; assert(battery.begin()); assert(!battery.hasBattery());
    assert(!battery.criticalProtectionRequired()); assert(!battery.processPowerButton());
    pwrLow = true; mockMillis = UINT32_MAX - 999; assert(!battery.processPowerButton());
    mockMillis = 999; assert(!battery.processPowerButton());
    mockMillis = 1000; assert(battery.processPowerButton());
}
static void shutdownTriesLatchThenReleasedWakeFallback() {
    for (bool external : {false, true}) {
        for (bool pressed : {false, true}) {
            reset(); externallyPowered = external;
            BatteryManager battery; assert(battery.begin()); pwrLow = pressed;
            bool terminated = false;
            try { (void)battery.powerOff(); }
            catch (const RailCut&) { assert(!external); terminated = true; }
            catch (const Slept& sleep) {
                assert(external);
                assert(sleep.mask == (1ULL << (pressed ? BOOT_BUTTON_GPIO : PWR_BUTTON_GPIO)));
                terminated = true;
            }
            assert(terminated && latchWrites == 1);
        }
    }
    reset(); latchWriteOk = false;
    BatteryManager battery; assert(battery.begin()); pwrLow = bootLow = true;
    assert(!battery.powerOff()); // Both asserted: no immediate-wake sleep loop.
    assert(latchWrites == 1);
}
int main() {
    bootHeldDoesNotBecomeUsb(); puts("PASS boot-held PWR retains protection, release then separate 2s hold works");
    inputDoesNotChangeVoltagePresenceOrProtection(); puts("PASS both supplies, no-cell/critical/normal ADC and hold/release preserve voltage policy");
    failedAdcStillPermitsReleasedThenHeldButton(); puts("PASS failed ADC preserves button, including clock rollover");
    shutdownTriesLatchThenReleasedWakeFallback(); puts("PASS actual shutdown owner cuts modeled battery or sleeps with inactive wake input; both-held aborts");
}
