#pragma once

#include <algorithm>
#include <cstdint>

inline int computeBrightnessSliderFill(uint8_t brightnessLevel, int sliderWidth) {
    const int fill = ((static_cast<int>(brightnessLevel) - 80) * sliderWidth) / 175;
    return std::clamp(fill, 0, sliderWidth);
}

inline int computeBrightnessSliderPercent(uint8_t brightnessLevel) {
    const int percent = ((static_cast<int>(brightnessLevel) - 80) * 100) / 175;
    return std::clamp(percent, 0, 100);
}
