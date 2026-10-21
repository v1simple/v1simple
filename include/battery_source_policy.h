#pragma once

#include <stdint.h>

// GPIO16/SYS_OUT is the physical PWR switch on both supported board revisions.
// Its released HIGH and pressed LOW levels do not distinguish USB from battery.
// Until a separate supply signal is qualified, BatteryManager reports Unknown.
// Unknown keeps conservative cell protection and the latch-then-sleep fallback;
// it must never be published as a measured battery/USB classification.
namespace battery_source_policy {

enum class Source : uint8_t { Unknown = 0, Battery = 1, Usb = 2 };

// Legacy policy boolean, not proof of the active supply. Only a qualified USB
// source could waive battery protection; unknown must not do so.
constexpr bool resolveOnBattery(Source source) { return source != Source::Usb; }

constexpr const char* sourceName(Source source) {
    return source == Source::Battery ? "battery" : (source == Source::Usb ? "usb" : "unknown");
}

struct ButtonGateInputs {
    bool managerInitialized = false;
    Source classification = Source::Unknown; // Observability only; PWR is independent of supply.
    bool adcHealthy = false;                 // Observability only.
    bool batteryVoltageValid = false;        // Observability only.
};

constexpr bool powerButtonHandlingEnabled(const ButtonGateInputs& inputs) {
    return inputs.managerInitialized;
}

} // namespace battery_source_policy
