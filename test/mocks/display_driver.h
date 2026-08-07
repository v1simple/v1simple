/**
 * Mock display_driver.h for native unit testing
 * Provides minimal stubs for Arduino_GFX display primitives
 */
#pragma once
#ifndef DISPLAY_DRIVER_H
#define DISPLAY_DRIVER_H

#ifdef ARDUINO
#include <Arduino.h>
#else
#include "Arduino.h"
#endif
#include <cstdint>
#include <algorithm>
#include <vector>
#include <string>
#include <cstring>
#include "pgmspace.h"   // PROGMEM (empty on native), pgm_read_*

// Text alignment datum constants (matches Adafruit GFX / Arduino_GFX)
#define TL_DATUM 0
#define TC_DATUM 1
#define TR_DATUM 2
#define ML_DATUM 3
#define MC_DATUM 4
#define MR_DATUM 5
#define BL_DATUM 6
#define BC_DATUM 7
#define BR_DATUM 8

// GFX font types (minimal stubs matching Adafruit GFX ABI)
struct GFXglyph {
    uint16_t bitmapOffset;
    uint8_t  width, height;
    uint8_t  xAdvance;
    int8_t   xOffset, yOffset;
};
struct GFXfont {
    uint8_t*  bitmap;
    GFXglyph* glyph;
    uint16_t  first, last;
    uint8_t   yAdvance;
};

// Color definitions (16-bit RGB565)
#define TFT_BLACK       0x0000
#define COLOR_RED       0xF800
#define COLOR_GREEN     0x07E0
#define COLOR_YELLOW    0xFFE0
#define TFT_WHITE       0xFFFF
#define TFT_RED         0xF800
#define TFT_GREEN       0x07E0
#define TFT_BLUE        0x001F
#define TFT_YELLOW      0xFFE0
#define TFT_CYAN        0x07FF
#define TFT_MAGENTA     0xF81F
#define TFT_ORANGE      0xFD20
#define TFT_GREY        0x8410
#define TFT_LIGHTGREY   0xC618
#define TFT_DARKGREY    0x1082  // Very dark grey (matches production display_driver.h)
#define TFT_GOLD        0xFEA0
#define TFT_SILVER      0xC618
#define TFT_PINK        0xFC18
#define TFT_PURPLE      0x8010
#define TFT_BROWN       0x8200

// Sentinel value for unconnected pins
#ifndef GFX_NOT_DEFINED
#define GFX_NOT_DEFINED (-1)
#endif

// AXS15231B panel init operations placeholder (native tests never execute it)
#ifndef axs15231b_180640_init_operations
static const uint8_t axs15231b_180640_init_operations[] = { 0x00 };
#endif

// Screen dimensions (Waveshare 3.49" rotated)
#ifndef SCREEN_WIDTH
#define SCREEN_WIDTH 640
#endif
#ifndef SCREEN_HEIGHT
#define SCREEN_HEIGHT 172
#endif
#ifndef CANVAS_WIDTH
#define CANVAS_WIDTH 172
#endif
#ifndef CANVAS_HEIGHT
#define CANVAS_HEIGHT 640
#endif

// Mock data bus
class Arduino_DataBus {
public:
    virtual ~Arduino_DataBus() = default;
};

class Arduino_ESP32QSPI : public Arduino_DataBus {
public:
    Arduino_ESP32QSPI(int cs, int sck, int d0, int d1, int d2, int d3) {}
};

// Mock GFX base class
class Arduino_GFX {
public:
    virtual ~Arduino_GFX() = default;
    virtual void drawPixel(int16_t x, int16_t y, uint16_t color) {}
    virtual void begin(int speed = 0) {}
    virtual void fillScreen(uint16_t color) {}
    virtual void drawRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {}
    virtual void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {}
    virtual void drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color) {}
    virtual void drawCircle(int16_t x, int16_t y, int16_t r, uint16_t color) {}
    virtual void fillCircle(int16_t x, int16_t y, int16_t r, uint16_t color) {}
    virtual void drawTriangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1, int16_t x2, int16_t y2, uint16_t color) {}
    virtual void fillTriangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1, int16_t x2, int16_t y2, uint16_t color) {}
    virtual void drawRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint16_t color) {}
    virtual void fillRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint16_t color) {}
    virtual void drawBitmap(int16_t, int16_t, const uint8_t*, int16_t, int16_t, uint16_t) {}
    virtual void draw16bitRGBBitmap(int16_t, int16_t, uint16_t*, int16_t, int16_t) {}
    virtual void setFont(const GFXfont* f) {}
    virtual void getTextBounds(const char* str, int16_t x, int16_t y,
                               int16_t* x1, int16_t* y1, uint16_t* w, uint16_t* h) {
        if (x1) *x1 = x;
        if (y1) *y1 = y;
        if (w)  *w  = static_cast<uint16_t>(strlen(str) * 8);
        if (h)  *h  = 10;
    }
    virtual void setTextWrap(bool wrap) {}
    virtual void setTextColor(uint16_t color) {}
    virtual void setTextColor(uint16_t fg, uint16_t bg) {}
    virtual void setTextSize(uint8_t size) {}
    virtual void setCursor(int16_t x, int16_t y) {}
    virtual void print(const char* s) {}
    virtual void print(int n) {}
    virtual void print(float n, int decimals = 2) {}
    virtual void println(const char* s = "") {}
    virtual int16_t width() { return SCREEN_WIDTH; }
    virtual int16_t height() { return SCREEN_HEIGHT; }
    virtual void flush() {}
};

// Mock AXS15231B display panel (12-arg ctor matching display.cpp usage)
class Arduino_AXS15231B : public Arduino_GFX {
public:
    Arduino_AXS15231B(Arduino_DataBus*, int, int, bool, int, int,
                      int = 0, int = 0, int = 0, int = 0,
                      const uint8_t* = nullptr, size_t = 0) {}
};

// Mock canvas for double-buffering
class Arduino_Canvas : public Arduino_GFX {
public:
    // GFX call recording — accessible for test assertions
    struct FillRectCall   { int16_t x, y, w, h; uint16_t color; };
    struct DrawRectCall   { int16_t x, y, w, h; uint16_t color; };
    struct FillRoundRectCall { int16_t x, y, w, h, r; uint16_t color; };
    struct FillCircleCall { int16_t x, y, r; uint16_t color; };
    struct DrawLineCall   { int16_t x0, y0, x1, y1; uint16_t color; };
    struct FillTriangleCall { int16_t x0, y0, x1, y1, x2, y2; uint16_t color; };

    std::vector<FillRectCall>      fillRectCalls;
    std::vector<DrawRectCall>      drawRectCalls;
    std::vector<FillRoundRectCall> fillRoundRectCalls;
    std::vector<FillCircleCall>    fillCircleCalls;
    std::vector<DrawLineCall>      drawLineCalls;
    std::vector<FillTriangleCall>  fillTriangleCalls;

    void clearRecordedCalls() {
        fillRectCalls.clear();
        drawRectCalls.clear();
        fillRoundRectCalls.clear();
        fillCircleCalls.clear();
        drawLineCalls.clear();
        fillTriangleCalls.clear();
    }

    Arduino_Canvas(int16_t w, int16_t h, Arduino_GFX* output, int16_t output_x = 0, int16_t output_y = 0)
        : w_(w),
          h_(h),
          output_(output),
          framebuffer_(static_cast<size_t>(CANVAS_WIDTH) * CANVAS_HEIGHT, 0),
          flushCount_(0),
          fillScreenCount_(0) {}

    void begin(int speed = 0) override {}

    void fillScreen(uint16_t color) override {
        lastFillColor_ = color;
        fillScreenCount_++;
        std::fill(framebuffer_.begin(), framebuffer_.end(), color);
    }

    void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) override {
        fillRectCalls.push_back({x, y, w, h, color});
        for (int16_t lx = x; lx < x + w; ++lx) {
            for (int16_t ly = y; ly < y + h; ++ly) {
                setLogicalPixel(lx, ly, color);
            }
        }
    }

    void drawRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) override {
        drawRectCalls.push_back({x, y, w, h, color});
    }

    void fillRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint16_t color) override {
        fillRoundRectCalls.push_back({x, y, w, h, r, color});
    }

    void fillCircle(int16_t x, int16_t y, int16_t r, uint16_t color) override {
        fillCircleCalls.push_back({x, y, r, color});
    }

    void drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color) override {
        drawLineCalls.push_back({x0, y0, x1, y1, color});
    }

    void fillTriangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                      int16_t x2, int16_t y2, uint16_t color) override {
        fillTriangleCalls.push_back({x0, y0, x1, y1, x2, y2, color});
    }

    void flush() override {
        flushCount_++;
    }

    uint16_t* getFramebuffer() { return framebuffer_.data(); }

    // Test helpers
    int getFlushCount() const { return flushCount_; }
    int getFillScreenCount() const { return fillScreenCount_; }
    uint16_t getLastFillColor() const { return lastFillColor_; }
    void resetCounters() { flushCount_ = 0; fillScreenCount_ = 0; clearRecordedCalls(); }

private:
    int16_t w_, h_;
    Arduino_GFX* output_;
    std::vector<uint16_t> framebuffer_;
    int flushCount_;
    int fillScreenCount_;
    uint16_t lastFillColor_ = 0;

    void setLogicalPixel(int16_t x, int16_t y, uint16_t color) {
        if (x < 0 || y < 0 || x >= SCREEN_WIDTH || y >= SCREEN_HEIGHT) {
            return;
        }
        const int16_t physX = static_cast<int16_t>(CANVAS_WIDTH - 1 - y);
        const int16_t physY = x;
        if (physX < 0 || physY < 0 || physX >= CANVAS_WIDTH || physY >= CANVAS_HEIGHT) {
            return;
        }
        framebuffer_[static_cast<size_t>(physY) * CANVAS_WIDTH + physX] = color;
    }
};

// OpenFontRender stub (for TTF fonts) — defined in OpenFontRender.h
#include "OpenFontRender.h"

#endif  // DISPLAY_DRIVER_H
