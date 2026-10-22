#include "battery_io.h"

static void tick(BatteryManager& battery, uint32_t time) { mockMillis = time; battery.update(); }
static void bootHeldDoesNotBecomeUsb() {
    resetBatteryIo(); pwrLow = true; voltageMv = 3100;
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
            resetBatteryIo(); externallyPowered = external; voltageMv = voltage;
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
    resetBatteryIo(); adcAvailable = false;
    BatteryManager battery; assert(battery.begin()); assert(!battery.hasBattery());
    assert(!battery.criticalProtectionRequired()); assert(!battery.processPowerButton());
    pwrLow = true; mockMillis = UINT32_MAX - 999; assert(!battery.processPowerButton());
    mockMillis = 999; assert(!battery.processPowerButton());
    mockMillis = 1000; assert(battery.processPowerButton());
}
static void shutdownTriesLatchThenReleasedWakeFallback() {
    for (bool external : {false, true}) {
        for (bool pressed : {false, true}) {
            resetBatteryIo(); externallyPowered = external;
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
    resetBatteryIo(); latchWriteOk = false;
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
