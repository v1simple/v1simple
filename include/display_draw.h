#pragma once

// Drawing wrappers expect each call site to provide `tft_`.

#include "display_driver.h" // DISPLAY_USE_ARDUINO_GFX, Arduino_Canvas, etc.

inline uint16_t dimColor(uint16_t c, uint8_t scalePercent = 60) {
    uint8_t r = (c >> 11) & 0x1F;
    uint8_t g = (c >> 5) & 0x3F;
    uint8_t b = c & 0x1F;
    r = (r * scalePercent) / 100;
    g = (g * scalePercent) / 100;
    b = (b * scalePercent) / 100;
    return (r << 11) | (g << 5) | b;
}

#define TFT_CALL(method) tft_->method
#define TFT_PTR tft_

// Hardware handles rotation; transforms are intentionally identity.
#define TX(vx, vy) (vx)
#define TY(vx, vy) (vy)
#define TW(vw, vh) (vw)
#define TH(vw, vh) (vh)

#define FILL_RECT(x, y, w, h, color) TFT_CALL(fillRect)(TX(x, y), TY(x, y), TW(w, h), TH(w, h), (color))
#define DRAW_RECT(x, y, w, h, color) TFT_CALL(drawRect)(TX(x, y), TY(x, y), TW(w, h), TH(w, h), (color))
#define FILL_ROUND_RECT(x, y, w, h, r, color)                                                                          \
    TFT_CALL(fillRoundRect)(TX(x, y), TY(x, y), TW(w, h), TH(w, h), (r), (color))
#define DRAW_ROUND_RECT(x, y, w, h, r, color)                                                                          \
    TFT_CALL(drawRoundRect)(TX(x, y), TY(x, y), TW(w, h), TH(w, h), (r), (color))
#define FILL_CIRCLE(x, y, r, color) TFT_CALL(fillCircle)(TX(x, y), TY(x, y), (r), (color))
#define DRAW_CIRCLE(x, y, r, color) TFT_CALL(drawCircle)(TX(x, y), TY(x, y), (r), (color))
#define FILL_TRIANGLE(x0, y0, x1, y1, x2, y2, color)                                                                   \
    TFT_CALL(fillTriangle)(TX(x0, y0), TY(x0, y0), TX(x1, y1), TY(x1, y1), TX(x2, y2), TY(x2, y2), (color))
#define DRAW_LINE(x0, y0, x1, y1, color) TFT_CALL(drawLine)(TX(x0, y0), TY(x0, y0), TX(x1, y1), TY(x1, y1), (color))
#define DRAW_PIXEL(x, y, color) TFT_CALL(drawPixel)(TX(x, y), TY(x, y), (color))
#define FILL_SCREEN(color) TFT_CALL(fillScreen)(color)
