#pragma once

#include <stdint.h>

namespace poweroff_policy {

enum class Strategy : uint8_t {
    HARD_LATCH_CUT = 0,
    DEEP_SLEEP_EXTERNAL_POWER = 1,
};

enum class WakeInput : uint8_t {
    PWR_GPIO16 = 0,
    BOOT_GPIO0 = 1,
};

struct WakePlan {
    WakeInput input = WakeInput::BOOT_GPIO0;
};

constexpr Strategy selectStrategy(bool onBattery) {
    // The TCA9554 latch can isolate the battery rail, but it cannot remove an
    // attached USB/external supply. External power therefore has an explicit
    // low-power sleep contract instead of pretending a physical cut occurred.
    return onBattery ? Strategy::HARD_LATCH_CUT : Strategy::DEEP_SLEEP_EXTERNAL_POWER;
}

constexpr const char* strategyName(Strategy strategy) {
    return strategy == Strategy::HARD_LATCH_CUT ? "hard_latch_cut" : "deep_sleep_external_power";
}

constexpr WakePlan planExternalPowerWake() {
    // Preserve the BOOT wake contract for a qualified external-supply plan.
    // GPIO16 is a physical button on both supplies, not an external-power
    // indicator. The hardware tail refuses sleep while the selected input is LOW.
    return {WakeInput::BOOT_GPIO0};
}

constexpr WakePlan planBatteryFallbackWake(bool pwrPinHigh) {
    if (pwrPinHigh) {
        return {WakeInput::PWR_GPIO16};
    }
    return {WakeInput::BOOT_GPIO0};
}

constexpr bool wakePlanIsInactive(const WakePlan& plan, bool pwrPinHigh, bool bootPinHigh) {
    return plan.input == WakeInput::PWR_GPIO16 ? pwrPinHigh : bootPinHigh;
}

constexpr const char* wakeInputName(WakeInput input) {
    return input == WakeInput::PWR_GPIO16 ? "PWR_GPIO16" : "BOOT_GPIO0";
}

} // namespace poweroff_policy
