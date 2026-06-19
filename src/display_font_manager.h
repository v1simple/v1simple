/**
 * DisplayFontManager — owns the OpenFontRender instance, init flag,
 * and glyph caches that were previously scattered as file-scope statics
 * in display.cpp.
 *
 * Lifetime: a single global instance (`fontMgr`) is defined in display.cpp
 *           and declared extern here for use by display sub-modules.
 *           init() is called once from V1Display::begin().
 *
 * Threading: same single-thread contract as the rest of the display system.
 */
#pragma once

#include "OpenFontRender.h"
#include <cstring>
#include <memory>

class Arduino_Canvas;

struct DisplayFontManager {

    // --- Layout constant shared with display drawing code ---
    static constexpr int TOP_COUNTER_FONT_SIZE = 60;

    // --- Renderer ---
    OpenFontRender segment7;

    // --- Init flag ---
    bool segment7Ready = false;

    // --- Top-counter glyph bounds cache ---
    static constexpr int16_t BOUNDS_INVALID =
        static_cast<int16_t>(-32768);
    int16_t topCounterXMin[128][2];
    int16_t topCounterXMax[128][2];
    bool    topCounterBoundsReady = false;

    // --- Lifecycle ---

    /// Load Segment7, prime the top-counter bounds cache.
    void init(Arduino_Canvas* canvas);
    void init(const std::unique_ptr<Arduino_Canvas>& canvas) { init(canvas.get()); }

    /// Pre-render common frequency glyphs once so first live alert draws
    /// don't stall while OpenFontRender builds glyph caches.
    void prewarmSegment7FrequencyGlyphs();

    // --- Top-counter bounds helpers ---

    void resetTopCounterBoundsCache();
    void primeTopCounterBoundsCache();
    bool getTopCounterBounds(char symbol, bool showDot, int& xMin, int& xMax);

    // --- Text width cache ---

    /// Small fixed-size LRU cache for OFR text widths.
    struct WidthCacheEntry {
        bool valid    = false;
        char text[16] = {0};
        int  width    = 0;
    };

    /// Look up (or compute + cache) the pixel width of @p text at @p fontSize.
    /// The cache is stored in the caller's local static array so that each
    /// rendering path maintains its own independent history.
    template <size_t N>
    static int cachedTextWidth(OpenFontRender& renderer, int fontSize,
                               const char* text,
                               WidthCacheEntry (&cache)[N],
                               uint8_t& nextSlot);

};

// --- Template implementation (must be visible to all callers) ---
template <size_t N>
int DisplayFontManager::cachedTextWidth(
        OpenFontRender& renderer, int fontSize, const char* text,
        WidthCacheEntry (&cache)[N], uint8_t& nextSlot) {

    for (size_t i = 0; i < N; ++i) {
        if (cache[i].valid && strcmp(cache[i].text, text) == 0) {
            return cache[i].width;
        }
    }

    renderer.setFontSize(fontSize);
    FT_BBox bbox = renderer.calculateBoundingBox(
        0, 0, fontSize, Align::Left, Layout::Horizontal, text);
    int width = bbox.xMax - bbox.xMin;

    WidthCacheEntry& dst = cache[nextSlot];
    dst.valid = true;
    strncpy(dst.text, text, sizeof(dst.text));
    dst.text[sizeof(dst.text) - 1] = '\0';
    dst.width = width;

    nextSlot = static_cast<uint8_t>((nextSlot + 1) % N);
    return width;
};
