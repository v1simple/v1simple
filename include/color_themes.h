/**
 * Color Palette Definitions for V1 Gen2 Display
 * Provides base colors for display elements (background, text, resting state)
 * Custom per-element colors are configurable via web UI and stored in settings
 * RGB565 format: RRRRR GGGGGG BBBBB (5 bits red, 6 bits green, 5 bits blue)
 */

#pragma once

#include <cstdint>

// Color palette structure - provides base display colors
struct ColorPalette {
    uint16_t bg;            // Background (black)
    uint16_t text;          // Text/foreground (white)
    uint16_t colorGray;     // Resting/inactive state (dark gray)
    uint16_t colorMuted;    // User-configurable muted-alert color (from settings)
    uint16_t colorPersisted; // User-configurable persisted-alert color (from settings)
};

namespace ColorThemes {
    // Standard color palette - the only palette used
    inline const ColorPalette& STANDARD() {
        static constexpr ColorPalette palette = {
            .bg = 0x0000,        // Black
            .text = 0xFFFF,      // White
            .colorGray = 0x1082  // Dark gray (resting)
        };
        return palette;
    }
}

