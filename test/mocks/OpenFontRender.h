#pragma once
// ============================================================================
// OpenFontRender stub for native unit tests.
// Provides the minimal interface needed so files that include OpenFontRender.h
// compile without the real ESP32 OpenFontRender library.
// ============================================================================

#ifndef OPENFONTRENDERH_H
#define OPENFONTRENDERH_H

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdarg>
#include <cstdio>

// FreeType error code
typedef int FT_Error;

// Forward-declare Arduino_GFX so setDrawer can accept a reference
class Arduino_GFX;

// Minimal FreeType bounding box stub
struct FT_BBox {
    long xMin = 0;
    long yMin = 0;
    long xMax = 0;
    long yMax = 0;
};

// Alignment / Layout enums used by DisplayFontManager
enum class Align { Left = 0, Center = 1, Right = 2 };
enum class Layout { Horizontal = 0, Vertical = 1 };

class OpenFontRender {
public:
    enum AlignEnum { ALIGN_LEFT = 0, ALIGN_CENTER = 1, ALIGN_RIGHT = 2 };

    int printfCount = 0;
    int16_t lastCursorX = 0;
    int16_t lastCursorY = 0;
    char lastPrinted[64] = "";

    void resetRecordedCalls() {
        printfCount = 0;
        lastCursorX = 0;
        lastCursorY = 0;
        lastPrinted[0] = '\0';
    }

    // Simulates the hardware failure mode where OFR's live
    // calculateBoundingBox returns a corrupted bounding box under FreeType
    // cache pressure (bench runs 8a599c91/1533471d: bogus xMin shifted the
    // top-counter digit 22 px). Applied to every subsequent measure; layout
    // code that consumes boot-primed cached bounds must be immune to it.
    // Deliberately NOT cleared by resetRecordedCalls() so a test can hold
    // the poison across draw helpers; tests reset it explicitly.
    int32_t bboxShiftX = 0;

    FT_Error loadFont(const uint8_t* /*fontData*/, size_t /*fontDataSize*/) { return 0; }
    void setDrawer(Arduino_GFX& /*gfx*/) {}
    void setCacheSize(int /*glyphN*/, int /*cacheN*/, size_t /*bytes*/) {}
    void setFontColor(uint16_t /*color*/) {}
    void setFontColor(uint16_t /*fg*/, uint16_t /*bg*/) {}
    void setFontColor(uint8_t /*r*/, uint8_t /*g*/, uint8_t /*b*/) {}
    void setBackgroundColor(uint8_t /*r*/, uint8_t /*g*/, uint8_t /*b*/) {}
    void setFontSize(float /*size*/) {}
    void setAlignment(AlignEnum /*align*/) {}
    void setCursor(int16_t x, int16_t y) {
        lastCursorX = x;
        lastCursorY = y;
    }
    void printf(const char* fmt, ...) {
        printfCount++;
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(lastPrinted, sizeof(lastPrinted), fmt, args);
        va_end(args);
        lastPrinted[sizeof(lastPrinted) - 1] = '\0';
    }
    int16_t getTextWidth(const char* text) { return static_cast<int16_t>(strlen(text) * 10); }
    int16_t getTextHeight(const char* /*text*/) { return 20; }
    FT_BBox calculateBoundingBox(int /*x*/, int /*y*/, int fontSize,
                                  Align /*align*/, Layout /*layout*/,
                                  const char* text) {
        FT_BBox box;
        box.xMin = 0;
        box.xMax = static_cast<long>(strlen(text) * fontSize / 2);
        // The real Segment7 TTF uses a fixed advance, but some glyphs have
        // narrow visible bounds (notably '1'). Model that for top-counter
        // tests so cursor anchoring bugs are visible on native.
        if (fontSize == 60 && text && text[0] && text[1] == '\0') {
            switch (text[0]) {
                case '1':
                    box.xMin = 24;
                    box.xMax = 32;
                    break;
                case 'L':
                case '&':
                case 'c':
                    box.xMin = 3;
                    box.xMax = 28;
                    break;
                default:
                    box.xMin = 3;
                    box.xMax = 32;
                    break;
            }
        }
        box.yMin = 0;
        box.yMax = fontSize;
        box.xMin += bboxShiftX;
        box.xMax += bboxShiftX;
        return box;
    }
};

#endif  // OPENFONTRENDERH_H
