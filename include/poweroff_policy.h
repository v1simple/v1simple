#pragma once

#include <stdint.h>

namespace poweroff_policy {

enum class WakeInput : uint8_t {
    PWR_GPIO16 = 0,
    BOOT_GPIO0 = 1,
};

struct WakePlan {
    WakeInput input = WakeInput::BOOT_GPIO0;
};

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
