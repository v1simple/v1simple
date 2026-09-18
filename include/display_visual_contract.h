#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "packet_parser.h"

namespace DisplayVisualContract {

// Used only when the V1's shared display image identifies Photo (P) but no
// alert row supplied a subtype. Values 8-15 remain available for the raw
// four-bit subtype field and are formatted explicitly below.
inline constexpr uint8_t PHOTO_TYPE_UNSPECIFIED = 0xFF;

inline const char* photoTypeShortLabel(uint8_t photoType, char* buffer, size_t bufferSize) {
    switch (photoType) {
        case 1: return "MRCT";
        case 2: return "3D";
        case 3: return "3DHD";
        case 4: return "HALO";
        case 5: return "NK7";
        case 6: return "EKIN";
        case 7: return "RT4";
        case PHOTO_TYPE_UNSPECIFIED: return "PHOTO";
        default:
            if (photoType >= 8 && photoType <= 15 && buffer && bufferSize > 0) {
                std::snprintf(buffer, bufferSize, "P%u", static_cast<unsigned>(photoType));
                return buffer;
            }
            return "";
    }
}

// Shared six-cell projection for live and preview renderers.
inline uint8_t projectVrBarsToSix(uint8_t vrBars) {
    const uint8_t clamped = (vrBars > 8) ? 8 : vrBars;
    return static_cast<uint8_t>((clamped * 6u + 4u) / 8u);
}

inline uint8_t alertMeterBars(const AlertData& alert) {
    uint8_t vrBars = 0;
    if (alert.direction & DIR_FRONT)
        vrBars = alert.frontStrength;
    else if (alert.direction & DIR_REAR)
        vrBars = alert.rearStrength;
    else
        vrBars = (alert.frontStrength > alert.rearStrength) ? alert.frontStrength : alert.rearStrength;
    return projectVrBarsToSix(vrBars);
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

// Compatibility translation for settings and API payloads written while the
// display exposed eight addressable colours. Runtime rendering stays six-cell.
inline void expandSixBarColorsToEight(const uint16_t configured[6], uint16_t out[8]) {
    // Preserve each physical color at the index selected by collapse below.
    // Interpolate only the two extra legacy cells, so a read/save is lossless.
    out[0] = configured[0];
    out[1] = configured[1];
    out[2] = lerpRgb565(configured[1], configured[2], 1, 2);
    out[3] = configured[2];
    out[4] = configured[3];
    out[5] = lerpRgb565(configured[3], configured[4], 1, 2);
    out[6] = configured[4];
    out[7] = configured[5];
}

inline void collapseEightBarColorsToSix(const uint16_t configured[8], uint16_t out[6]) {
    static constexpr uint8_t kSourceIndex[6] = {0, 1, 3, 4, 6, 7};
    for (int i = 0; i < 6; ++i) {
        out[i] = configured[kSourceIndex[i]];
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
