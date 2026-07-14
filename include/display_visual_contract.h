#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "packet_parser.h"

namespace DisplayVisualContract {

inline uint8_t scalePreviewBarsToMainMeter(uint8_t previewBars) {
    if (previewBars > 6)
        previewBars = 6;
    return static_cast<uint8_t>((previewBars * 8u + 3u) / 6u);
}

inline uint16_t lerpRgb565(uint16_t a, uint16_t b, uint8_t num, uint8_t den) {
    const int ar = (a >> 11) & 0x1F;
    const int ag = (a >> 5) & 0x3F;
    const int ab = a & 0x1F;
    const int br = (b >> 11) & 0x1F;
    const int bg = (b >> 5) & 0x3F;
    const int bb = b & 0x1F;
    const int r = ar + (((br - ar) * num) + den / 2) / den;
    const int g = ag + (((bg - ag) * num) + den / 2) / den;
    const int bl = ab + (((bb - ab) * num) + den / 2) / den;
    return static_cast<uint16_t>((r << 11) | (g << 5) | bl);
}

inline void buildMainMeterRamp(const uint16_t configured[6], uint16_t out[8]) {
    for (int i = 0; i < 8; ++i) {
        const int scaled = i * 5;
        const int idx = scaled / 7;
        const int rem = scaled % 7;
        out[i] = (rem == 0 || idx >= 5)
                     ? configured[idx]
                     : lerpRgb565(configured[idx], configured[idx + 1], static_cast<uint8_t>(rem), 7);
    }
}

inline uint8_t bandCellMask(uint8_t activeBandMask, int index) {
    if (index == 2 && (activeBandMask & BAND_KU) != 0) {
        return static_cast<uint8_t>(BAND_K | BAND_KU);
    }
    return (index == 0) ? BAND_LASER : ((index == 1) ? BAND_KA : ((index == 2) ? BAND_K : BAND_X));
}

inline const char* bandCellLabel(uint8_t activeBandMask, int index) {
    if (index == 2 && (activeBandMask & BAND_KU) != 0) {
        return "Ku";
    }
    return (index == 0) ? "L" : ((index == 1) ? "Ka" : ((index == 2) ? "K" : "X"));
}

inline const char* frequencyTextForAlert(Band band, uint32_t freqMHz, const char* alpGunAbbrev, char* buffer,
                                         size_t bufferSize) {
    if (alpGunAbbrev && alpGunAbbrev[0] != '\0') {
        return alpGunAbbrev;
    }
    if (band == BAND_LASER) {
        return "LASER";
    }
    if (freqMHz == 0) {
        return "--.---";
    }
    if (!buffer || bufferSize == 0) {
        return "";
    }
    const unsigned long whole = static_cast<unsigned long>(freqMHz / 1000u);
    const unsigned long frac = static_cast<unsigned long>(freqMHz % 1000u);
    std::snprintf(buffer, bufferSize, "%lu.%03lu", whole, frac);
    return buffer;
}

} // namespace DisplayVisualContract
