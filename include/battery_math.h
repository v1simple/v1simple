#pragma once

#include <stdint.h>

namespace battery_math {

inline constexpr uint16_t kFullMv = 4095;
inline constexpr uint16_t kEmptyMv = 3200;
inline constexpr uint16_t kWarningMv = 3400;
inline constexpr uint16_t kCriticalMv = 3250;

inline uint8_t voltageToPercent(uint16_t voltageMV) {
    if (voltageMV >= kFullMv) {
        return 100;
    }
    if (voltageMV <= kEmptyMv) {
        return 0;
    }
    return static_cast<uint8_t>((static_cast<uint32_t>(voltageMV - kEmptyMv) * 100U) / (kFullMv - kEmptyMv));
}

inline bool isLow(uint16_t voltageMV) {
    return voltageMV < kWarningMv && voltageMV > 0;
}

inline bool isCritical(uint16_t voltageMV) {
    return voltageMV < kCriticalMv && voltageMV > 0;
}

// Critical protection deliberately has a different contract from the battery
// icon's presence heuristic. Once the source classifier says the device is on
// the cell, every nonzero reading below the critical threshold is actionable;
// 0 remains the ADC-invalid/no-reading sentinel.
inline bool criticalProtectionRequired(bool sourceIsBattery, uint16_t voltageMV) {
    return sourceIsBattery && isCritical(voltageMV);
}

} // namespace battery_math
