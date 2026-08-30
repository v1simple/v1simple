#pragma once


#include "color_themes.h" // ColorPalette, ColorThemes

class V1Display;

// Active display for palette and persisted-mode lookup.
extern V1Display* g_displayInstance;

// The complete V1Display definition must be visible before including this header.
inline const ColorPalette& getColorPalette() {
    if (g_displayInstance) {
        return g_displayInstance->getCurrentPalette();
    }
    return ColorThemes::STANDARD();
}

#define PALETTE_BG getColorPalette().bg
#define PALETTE_TEXT getColorPalette().text
#define PALETTE_GRAY getColorPalette().colorGray
#define PALETTE_MUTED getColorPalette().colorMuted
#define PALETTE_PERSISTED getColorPalette().colorPersisted

#define PALETTE_MUTED_OR_PERSISTED                                                                                     \
    (g_displayInstance && g_displayInstance->isPersistedMode() ? PALETTE_PERSISTED : PALETTE_MUTED)

// OpenFontRender accepts RGB888; native display state is RGB565.
struct Rgb888 {
    uint8_t r;
    uint8_t g;
    uint8_t b;
};

inline constexpr Rgb888 rgb565ToRgb888(uint16_t c) {
    return Rgb888{static_cast<uint8_t>((c >> 11) << 3), static_cast<uint8_t>(((c >> 5) & 0x3F) << 2),
                  static_cast<uint8_t>((c & 0x1F) << 3)};
}
