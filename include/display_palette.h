#pragma once

// ============================================================================
// Display Palette — colour helpers shared by display sub-modules
//
// Provides the current theme palette and user-configurable colours so that
// drawing code extracted from display.cpp (e.g. display_arrow.cpp) can
// resolve colours without depending on display.cpp-local state.
// ============================================================================

#include "color_themes.h" // ColorPalette, ColorThemes

// Forward-declare V1Display (full definition not needed for the pointer)
class V1Display;

// Global display instance pointer — set by V1Display constructor, defined in
// display.cpp.  Used to reach the active colour palette and persisted-mode flag.
extern V1Display* g_displayInstance;

// Resolve the current colour palette (falls back to STANDARD if no instance).
// NOTE: must be defined after V1Display::getCurrentPalette() is visible.  In
// display.cpp that's already the case; extracted .cpp files include display.h
// before this header so the full class definition is available.
inline const ColorPalette& getColorPalette() {
    if (g_displayInstance) {
        return g_displayInstance->getCurrentPalette();
    }
    return ColorThemes::STANDARD();
}

// Convenience macros — evaluate to the live palette / user colours.
#define PALETTE_BG getColorPalette().bg
#define PALETTE_TEXT getColorPalette().text
#define PALETTE_GRAY getColorPalette().colorGray
#define PALETTE_MUTED getColorPalette().colorMuted
#define PALETTE_PERSISTED getColorPalette().colorPersisted

// Returns PALETTE_PERSISTED when in persisted-alert mode, else PALETTE_MUTED.
#define PALETTE_MUTED_OR_PERSISTED                                                                                     \
    (g_displayInstance && g_displayInstance->isPersistedMode() ? PALETTE_PERSISTED : PALETTE_MUTED)

// --- RGB565 → RGB888 helper ----------------------------------------------
// OpenFontRender's setFontColor / setBackgroundColor take 8-bit components.
// Native draw state is RGB565, so every glyph draw would otherwise inline the
// same bit-twiddle (see display_top_counter.cpp, display_frequency.cpp).
// Factor it into one constexpr helper so:
//   - there is one definition of "how we unpack a 565 to 888"
//   - the compiler can constant-fold the shifts for literal colors (e.g. the
//     STANDARD palette bg = 0x0000 → {0,0,0} at compile time)
struct Rgb888 {
    uint8_t r;
    uint8_t g;
    uint8_t b;
};

inline constexpr Rgb888 rgb565ToRgb888(uint16_t c) {
    return Rgb888{static_cast<uint8_t>((c >> 11) << 3), static_cast<uint8_t>(((c >> 5) & 0x3F) << 2),
                  static_cast<uint8_t>((c & 0x1F) << 3)};
}
