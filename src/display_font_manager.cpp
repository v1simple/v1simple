#include "display_font_manager.h"
#include "display_driver.h" // Arduino_Canvas full definition (via Arduino_GFX_Library)
#include "display_layout.h"
#include "Segment7Font.h" // Modified V1SevenX TTF binary data
#include <Arduino.h>      // millis(), Serial, psramFound()
#include <esp_heap_caps.h>

void DisplayFontManager::init(Arduino_Canvas* canvas) {
    if (!canvas) {
        Serial.println("[FontMgr] ERROR: null canvas in init()");
        return;
    }

    // Frequency and counter share this cache; use the smaller budget without PSRAM.
    const bool psramOk = psramFound() && (ESP.getPsramSize() > 0);
    const uint32_t seg7Cache = psramOk ? 98304u : 8192u; // 96 KB

    Serial.printf("[FontMgr] cache budget: psram=%s seg7=%lu\n", psramOk ? "yes" : "no",
                  static_cast<unsigned long>(seg7Cache));

    segment7.setDrawer(*canvas);
    segment7.setCacheSize(1, 4, seg7Cache);
    FT_Error err = segment7.loadFont(V1SevenXFont, sizeof(V1SevenXFont));
    segment7Ready = (err == 0);
    if (err) {
        Serial.printf("[FontMgr] ERROR: Segment7 load failed (0x%02X)\n", err);
    }

    // Prime bounds before the first frame so counter alignment is stable.
    primeTopCounterBoundsCache();
    prewarmSegment7FrequencyGlyphs();

#ifdef CONFIG_SPIRAM_SUPPORT
    Serial.println("[FontMgr] OFR using PSRAM-preferring allocator (ps_malloc)");
#else
    Serial.println("[FontMgr] OFR using internal heap allocator (malloc)");
#endif

    Serial.printf("[FontMgr] OK font(seg7)=%d\n", segment7Ready);
}

void DisplayFontManager::prewarmSegment7FrequencyGlyphs() {
    if (!segment7Ready) {
        return;
    }

    // Avoid glyph-build latency on the first live alert.
    static constexpr int kWarmFontSize = DisplayLayout::FREQUENCY_OFR_FONT_SIZE;
    static constexpr const char* kWarmupSamples[] = {
        "33.800", "35.500", "34.700", "24.150", "10.525", "88.888", "--.---", "LASER", "truSPd", "drgEYE",
    };

    const unsigned long warmStartMs = millis();
    segment7.setBackgroundColor(0, 0, 0);
    segment7.setFontColor(0, 0, 0); // Render black-on-black into canvas.
    segment7.setFontSize(kWarmFontSize);

    for (const char* sample : kWarmupSamples) {
        // Bound-box pass and draw pass together prime both metric and glyph caches.
        segment7.calculateBoundingBox(0, 0, kWarmFontSize, Align::Left, Layout::Horizontal, sample);
        segment7.setCursor(2, kWarmFontSize);
        segment7.printf("%s", sample);
    }

    Serial.printf("[FontMgr] Segment7 prewarm complete in %lu ms\n", millis() - warmStartMs);
}

void DisplayFontManager::resetTopCounterBoundsCache() {
    for (uint8_t c = 0; c < 128; ++c) {
        topCounterXMin[c][0] = BOUNDS_INVALID;
        topCounterXMin[c][1] = BOUNDS_INVALID;
        topCounterXMax[c][0] = BOUNDS_INVALID;
        topCounterXMax[c][1] = BOUNDS_INVALID;
    }
    topCounterBoundsReady = false;
}

void DisplayFontManager::primeTopCounterBoundsCache() {
    resetTopCounterBoundsCache();
    if (!segment7Ready) {
        return;
    }

    // Prime only symbols emitted by the V1 counter decoder.
    static constexpr char kTopCounterSymbols[] = " 0123456789AJLPCUEF#&bcdu";

    segment7.setFontSize(TOP_COUNTER_FONT_SIZE);
    for (const char symbol : kTopCounterSymbols) {
        if (symbol == '\0') {
            break;
        }
        const uint8_t c = static_cast<uint8_t>(symbol);
        if (c >= 128) {
            continue;
        }

        // Cache both plain and dotted variants for each supported symbol.
        auto cacheBounds = [&](bool showDot) {
            char text[3] = {symbol, 0, 0};
            if (showDot) {
                text[1] = '.';
            }
            FT_BBox bbox =
                segment7.calculateBoundingBox(0, 0, TOP_COUNTER_FONT_SIZE, Align::Left, Layout::Horizontal, text);
            int xMin = static_cast<int>(bbox.xMin);
            int xMax = static_cast<int>(bbox.xMax);
            if (xMin < -32767)
                xMin = -32767;
            if (xMin > 32767)
                xMin = 32767;
            if (xMax < -32767)
                xMax = -32767;
            if (xMax > 32767)
                xMax = 32767;
            topCounterXMin[c][showDot ? 1 : 0] = static_cast<int16_t>(xMin);
            topCounterXMax[c][showDot ? 1 : 0] = static_cast<int16_t>(xMax);
        };

        cacheBounds(false);
        cacheBounds(true);
    }
    topCounterBoundsReady = true;
}

bool DisplayFontManager::getTopCounterBounds(char symbol, bool showDot, int& xMin, int& xMax) {
    const uint8_t idx = static_cast<uint8_t>(symbol);
    const uint8_t dotIdx = showDot ? 1 : 0;

    if (topCounterBoundsReady && idx < 128) {
        int16_t cachedMin = topCounterXMin[idx][dotIdx];
        int16_t cachedMax = topCounterXMax[idx][dotIdx];
        if (cachedMin != BOUNDS_INVALID && cachedMax != BOUNDS_INVALID) {
            xMin = cachedMin;
            xMax = cachedMax;
            return true;
        }
    }

    if (!segment7Ready) {
        return false;
    }

    char text[3] = {symbol, 0, 0};
    if (showDot) {
        text[1] = '.';
    }
    FT_BBox bbox = segment7.calculateBoundingBox(0, 0, TOP_COUNTER_FONT_SIZE, Align::Left, Layout::Horizontal, text);
    xMin = static_cast<int>(bbox.xMin);
    xMax = static_cast<int>(bbox.xMax);
    return true;
}
