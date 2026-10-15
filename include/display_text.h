#pragma once

// Emulate TFT_eSPI text alignment on Arduino_Canvas.

#include "display_driver.h"
#include <memory>

// All text helpers share one datum for the single display context.
inline uint8_t& gfxCurrentTextDatum() {
    static uint8_t datum = TL_DATUM;
    return datum;
}

#define GFX_setTextDatum(d)                                                                                            \
    do {                                                                                                               \
        gfxCurrentTextDatum() = (d);                                                                                   \
    } while (0)

inline void GFX_drawString(Arduino_Canvas* canvas, const char* str, int16_t x, int16_t y) {
    int16_t x1, y1;
    uint16_t w, h;
    canvas->getTextBounds(str, 0, 0, &x1, &y1, &w, &h);

    int16_t drawX = x, drawY = y;
    const uint8_t datum = gfxCurrentTextDatum();

    // Horizontal alignment
    switch (datum) {
    case TC_DATUM:
    case MC_DATUM:
    case BC_DATUM:
        drawX = x - x1 - w / 2;
        break;
    case TR_DATUM:
    case MR_DATUM:
    case BR_DATUM:
        drawX = x - x1 - w;
        break;
    default: // TL, ML, BL — left aligned
        drawX = x - x1;
        break;
    }

    // Vertical alignment
    switch (datum) {
    case ML_DATUM:
    case MC_DATUM:
    case MR_DATUM:
        drawY = y - y1 - h / 2;
        break;
    case BL_DATUM:
    case BC_DATUM:
    case BR_DATUM:
        drawY = y - y1 - h;
        break;
    default: // TL, TC, TR — top aligned
        drawY = y - y1;
        break;
    }

    canvas->setCursor(drawX, drawY);
    canvas->print(str);
}

inline void GFX_drawString(const std::unique_ptr<Arduino_Canvas>& canvas, const char* str, int16_t x, int16_t y) {
    GFX_drawString(canvas.get(), str, x, y);
}
