// Owned by V1Display and initialized in begin(); rendering is main-loop-only.
#pragma once

#include "OpenFontRender.h"
#include <cstring>
#include <memory>

class Arduino_Canvas;

struct DisplayFontManager {

    static constexpr int TOP_COUNTER_FONT_SIZE = 60;

    OpenFontRender segment7;

    bool segment7Ready = false;

    static constexpr int16_t BOUNDS_INVALID = static_cast<int16_t>(-32768);
    int16_t topCounterXMin[128][2];
    int16_t topCounterXMax[128][2];
    bool topCounterBoundsReady = false;

    /// Load Segment7, prime the top-counter bounds cache.
    void init(Arduino_Canvas* canvas);
    void init(const std::unique_ptr<Arduino_Canvas>& canvas) { init(canvas.get()); }

    /// Pre-render common frequency glyphs once so first live alert draws
    /// don't stall while OpenFontRender builds glyph caches.
    void prewarmSegment7FrequencyGlyphs();

    void resetTopCounterBoundsCache();
    void primeTopCounterBoundsCache();
    bool getTopCounterBounds(char symbol, bool showDot, int& xMin, int& xMax);

    // Fixed-size round-robin cache for OFR text widths.
    struct WidthCacheEntry {
        bool valid = false;
        char text[16] = {0};
        int width = 0;
    };

    // Each rendering path owns its cache; entries assume one font and size.
    template <size_t N>
    static int cachedTextWidth(OpenFontRender& renderer, int fontSize, const char* text, WidthCacheEntry (&cache)[N],
                               uint8_t& nextSlot);
};

template <size_t N>
int DisplayFontManager::cachedTextWidth(OpenFontRender& renderer, int fontSize, const char* text,
                                        WidthCacheEntry (&cache)[N], uint8_t& nextSlot) {

    for (size_t i = 0; i < N; ++i) {
        if (cache[i].valid && strcmp(cache[i].text, text) == 0) {
            return cache[i].width;
        }
    }

    renderer.setFontSize(fontSize);
    FT_BBox bbox = renderer.calculateBoundingBox(0, 0, fontSize, Align::Left, Layout::Horizontal, text);
    int width = bbox.xMax - bbox.xMin;

    WidthCacheEntry& dst = cache[nextSlot];
    dst.valid = true;
    strncpy(dst.text, text, sizeof(dst.text));
    dst.text[sizeof(dst.text) - 1] = '\0';
    dst.width = width;

    nextSlot = static_cast<uint8_t>((nextSlot + 1) % N);
    return width;
};
