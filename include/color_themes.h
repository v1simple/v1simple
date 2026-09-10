/**
 * Color Palette Definitions for V1 Gen2 Display
 * Provides base colors for display elements (background, text, resting state)
 * Custom per-element colors are configurable via web UI and stored in settings
 * RGB565 format: RRRRR GGGGGG BBBBB (5 bits red, 6 bits green, 5 bits blue)
 */

#pragma once

#include <cstdint>

// One colour per signal-bar segment. Matches DisplayLayout::MAIN_SIGNAL_BAR_COUNT
// and CARD_METER_BAR_COUNT; both meters are six segments and each segment is
// individually addressable, with no interpolation at draw time.
inline constexpr int SIGNAL_BAR_COLOR_COUNT = 6;

// Color palette structure - provides base display colors
struct ColorPalette {
    uint16_t bg;             // Background (black)
    uint16_t text;           // Text/foreground (white)
    uint16_t colorGray;      // Resting/inactive state (dark gray)
    uint16_t colorMuted = 0x4A49;     // Subdued grey until settings override it
    uint16_t colorPersisted = 0x4208; // Subdued grey until settings override it
};

namespace ColorThemes {
// Standard color palette - the only palette used
inline const ColorPalette& STANDARD() {
    static constexpr ColorPalette palette = {
        .bg = 0x0000,       // Black
        .text = 0xFFFF,     // White
        .colorGray = 0x3186 // Subdued gray (resting)
    };
    return palette;
}
} // namespace ColorThemes
