/**
 * Frequency display rendering — extracted from display.cpp (Phase 2M)
 *
 * Contains the Segment7 frequency renderer and volume-zero warning.
 */

#include "display.h"
#include "display_layout.h"
#include "display_draw.h"
#include "display_element_caches.h"
#include "display_palette.h"
#include "display_text.h"
#include "display_segments.h"
#include "display_font_manager.h"
#include "settings.h"
#include "perf_metrics.h"
#include <algorithm>
#include <cstring>

using namespace DisplaySegments;
// Convenience alias (matches display.cpp)
using TextWidthCacheEntry = DisplayFontManager::WidthCacheEntry;

// File-scoped font width caches for frequency displays.
// Round-robin computation caches — not render state; render caches are in
// elementCaches_.  32 entries keeps eviction rare in busy Ka/K environments
// where many distinct frequency strings cycle through.  Cost: ~768 bytes BSS
// per cache (WidthCacheEntry is ~24 bytes with alignment).
static TextWidthCacheEntry s_frequencyWidthCache[32];
static uint8_t s_frequencyWidthCacheNextSlot = 0;
static int s_frequencyCachedNumericWidth = 0;
static int s_frequencyCachedDashWidth = 0;

struct FrequencyBoundsCacheEntry {
    bool valid = false;
    char text[16] = {0};
    int fontSize = 0;
    int xMin = 0;
    int xMax = 0;
};
static FrequencyBoundsCacheEntry s_frequencyBoundsCache[16];
static uint8_t s_frequencyBoundsCacheNextSlot = 0;

namespace {
constexpr uint8_t kFrequencyRasterFlagOfr = 0x01;
constexpr uint8_t kFrequencyRasterFlagMuted = 0x02;
constexpr uint8_t kFrequencyRasterFlagPhoto = 0x04;
constexpr uint8_t kFrequencyRasterFlagAlp = 0x08;
constexpr uint8_t kFrequencyRasterFlagLaser = 0x10;

uint8_t frequencyRasterFlags(bool muted, bool isPhotoRadar, bool isAlpOverride, bool isLaser) {
    uint8_t flags = kFrequencyRasterFlagOfr;
    if (muted)
        flags |= kFrequencyRasterFlagMuted;
    if (isPhotoRadar)
        flags |= kFrequencyRasterFlagPhoto;
    if (isAlpOverride)
        flags |= kFrequencyRasterFlagAlp;
    if (isLaser)
        flags |= kFrequencyRasterFlagLaser;
    return flags;
}

FrequencyRasterKey makeFrequencyRasterKey(const char* text, uint16_t color, uint16_t bg, uint16_t fontSize,
                                          uint32_t paletteRevision, int x, int y, int w, int h, uint8_t flags) {
    FrequencyRasterKey key{};
    std::strncpy(key.text, text, sizeof(key.text));
    key.text[sizeof(key.text) - 1] = '\0';
    key.color = color;
    key.bg = bg;
    key.fontSize = fontSize;
    key.paletteRevision = paletteRevision;
    key.x = static_cast<int16_t>(x);
    key.y = static_cast<int16_t>(y);
    key.w = static_cast<int16_t>(w);
    key.h = static_cast<int16_t>(h);
    key.flags = flags;
    return key;
}

void getFrequencyTextBounds(DisplayFontManager& fontMgr, int fontSize, const char* text, int& xMin, int& xMax) {
    for (FrequencyBoundsCacheEntry& entry : s_frequencyBoundsCache) {
        if (entry.valid && entry.fontSize == fontSize && std::strncmp(entry.text, text, sizeof(entry.text)) == 0) {
            xMin = entry.xMin;
            xMax = entry.xMax;
            return;
        }
    }

    fontMgr.segment7.setFontSize(fontSize);
    FT_BBox bbox = fontMgr.segment7.calculateBoundingBox(0, 0, fontSize, Align::Left, Layout::Horizontal, text);
    xMin = static_cast<int>(bbox.xMin);
    xMax = static_cast<int>(bbox.xMax);

    FrequencyBoundsCacheEntry& dst = s_frequencyBoundsCache[s_frequencyBoundsCacheNextSlot];
    dst.valid = true;
    dst.fontSize = fontSize;
    std::strncpy(dst.text, text, sizeof(dst.text));
    dst.text[sizeof(dst.text) - 1] = '\0';
    dst.xMin = xMin;
    dst.xMax = xMax;
    s_frequencyBoundsCacheNextSlot = static_cast<uint8_t>(
        (s_frequencyBoundsCacheNextSlot + 1) % (sizeof(s_frequencyBoundsCache) / sizeof(s_frequencyBoundsCache[0])));
}
} // namespace

// --- Segment7 frequency display. Uses Segment7 TTF font if available, falls back to software renderer. ---

void V1Display::drawFrequencySegment7(uint32_t freqMHz, Band band, bool muted, bool isPhotoRadar) {
    const V1Settings& s = settingsManager.get();

    const bool usingOfr = fontMgr_.segment7Ready;
    const bool hasFreq = freqMHz > 0;

    char textBuf[16];
    bool isAlpOverride = false;
    if (alpFreqOverride_ && alpFreqText_[0] != '\0') {
        // ALP gun abbreviation overrides frequency text
        strncpy(textBuf, alpFreqText_, sizeof(textBuf));
        textBuf[sizeof(textBuf) - 1] = '\0';
        isAlpOverride = true;
    } else if (band == BAND_LASER) {
        snprintf(textBuf, sizeof(textBuf), "LASER");
    } else if (hasFreq) {
        float freqGhz = freqMHz / 1000.0f;
        snprintf(textBuf, sizeof(textBuf), "%05.3f", freqGhz);
    } else {
        snprintf(textBuf, sizeof(textBuf), "--.---");
    }

    uint16_t freqColor;
    if (isAlpOverride) {
        // LID active (above LID speed limit) uses colorAlpLidActive; otherwise
        // DLI active uses colorAlpDli. Naming matches AL Priority manual.
        freqColor = muted ? PALETTE_MUTED_OR_PERSISTED : (alpLidActive_ ? s.colorAlpLidActive : s.colorAlpDli);
    } else if (usingOfr) {
        if (band == BAND_LASER) {
            freqColor = muted ? PALETTE_MUTED_OR_PERSISTED : s.colorBandL;
        } else if (muted) {
            freqColor = PALETTE_MUTED_OR_PERSISTED;
        } else if (!hasFreq) {
            freqColor = PALETTE_GRAY;
        } else if (isPhotoRadar && s.freqUseBandColor) {
            freqColor = s.colorBandPhoto; // Photo radar gets its own color
        } else if (s.freqUseBandColor && band != BAND_NONE) {
            freqColor = getBandColor(band);
        } else {
            freqColor = s.colorFrequency;
        }
    } else {
        // Keep fallback color behavior unchanged.
        if (band == BAND_LASER) {
            freqColor = muted ? PALETTE_MUTED_OR_PERSISTED : s.colorBandL;
        } else if (muted) {
            freqColor = PALETTE_MUTED_OR_PERSISTED;
        } else if (!hasFreq) {
            freqColor = PALETTE_GRAY;
        } else if (s.freqUseBandColor && band != BAND_NONE) {
            freqColor = getBandColor(band);
        } else {
            freqColor = s.colorFrequency;
        }
    }

    bool textChanged = (strcmp(elementCaches_.frequency.lastText, textBuf) != 0);
    bool changed = !elementCaches_.frequency.valid || (elementCaches_.frequency.lastUsedOfr != usingOfr) ||
                   textChanged || (elementCaches_.frequency.lastColor != freqColor);
    if (!changed) {
        return;
    }
    perfRecordDisplayRedrawReason(PerfDisplayRedrawReason::FrequencyChange);

    if (usingOfr) {
        // Use Segment7 TTF font
        const int fontSize = DisplayLayout::FREQUENCY_OFR_FONT_SIZE;
        const int leftMargin = DisplayLayout::FREQUENCY_OFR_LEFT_MARGIN;
        const int y = DisplayLayout::frequencyOfrY();
        const int maxWidth = DisplayLayout::frequencyOfrMaxWidth();
        if (s_frequencyCachedNumericWidth <= 0) {
            s_frequencyCachedNumericWidth = DisplayFontManager::cachedTextWidth(
                fontMgr_.segment7, fontSize, "88.888", s_frequencyWidthCache, s_frequencyWidthCacheNextSlot);
        }
        if (s_frequencyCachedDashWidth <= 0) {
            s_frequencyCachedDashWidth = DisplayFontManager::cachedTextWidth(
                fontMgr_.segment7, fontSize, "--.---", s_frequencyWidthCache, s_frequencyWidthCacheNextSlot);
        }

        int textWidth = s_frequencyCachedNumericWidth;
        int glyphXMin = 0;
        int glyphXMax = textWidth;
        if (isAlpOverride) {
            // ALP gun abbreviations vary and may have left/right bearings.
            getFrequencyTextBounds(fontMgr_, fontSize, textBuf, glyphXMin, glyphXMax);
            textWidth = glyphXMax - glyphXMin;
        } else if (band == BAND_LASER) {
            // LASER's leading glyph overhangs the nominal cursor box in Segment7.
            // Clear against the real glyph bounds so stale edge pixels disappear
            // when returning from LASER to numeric frequencies.
            getFrequencyTextBounds(fontMgr_, fontSize, textBuf, glyphXMin, glyphXMax);
            textWidth = glyphXMax - glyphXMin;
        } else if (!hasFreq) {
            textWidth = s_frequencyCachedDashWidth;
            glyphXMax = textWidth;
        }

        int x = leftMargin + (maxWidth - textWidth) / 2;
        if (x < leftMargin)
            x = leftMargin;

        // Clear only the union of old/new glyph bounds.  Store lastDrawX/Width as
        // actual glyph bounds rather than the cursor box; Segment7 alpha glyphs
        // such as LASER can overhang the cursor and otherwise leave edge pixels.
        constexpr int kClearPadPx = 12;
        int clearY = y - 8;
        int clearH = fontSize + 16;
        const int maxClearBottom = DisplayLayout::CONTENT_BOTTOM_Y;
        if (clearY + clearH > maxClearBottom)
            clearH = maxClearBottom - clearY;
        int clearLeft = x + glyphXMin - kClearPadPx;
        int clearRight = x + glyphXMax + kClearPadPx;
        if (elementCaches_.frequency.valid && elementCaches_.frequency.lastUsedOfr &&
            elementCaches_.frequency.lastDrawWidth > 0) {
            clearLeft = std::min(clearLeft, elementCaches_.frequency.lastDrawX - kClearPadPx);
            clearRight = std::max(clearRight, elementCaches_.frequency.lastDrawX +
                                                  elementCaches_.frequency.lastDrawWidth + kClearPadPx);
        }
        const int clearMinX = DisplayLayout::kFrequencyZoneRect.x;
        const int clearMaxX = DisplayLayout::kFrequencyZoneRect.x + DisplayLayout::kFrequencyZoneRect.w;
        clearLeft = std::max(clearLeft, clearMinX);
        clearRight = std::min(clearRight, clearMaxX);
        const int clearW = clearRight - clearLeft;
        const int glyphLeft = std::max(x + glyphXMin - kClearPadPx, clearMinX);
        const int glyphRight = std::min(x + glyphXMax + kClearPadPx, clearMaxX);
        const int glyphW = glyphRight - glyphLeft;

        uint16_t* framebuffer = nullptr;
        const bool numericAtlasEligible = hasFreq && !isAlpOverride && band != BAND_LASER &&
                                          DisplayFrequencyDigitAtlas::isNumericFrequencyText(textBuf);
        const bool digitAtlasReady = numericAtlasEligible && frequencyDigitAtlas_.ready();
        bool rasterKeyValid = false;
        FrequencyRasterKey rasterKey{};
        if (tft_ && (digitAtlasReady || frequencyRasterCache_.enabled())) {
            framebuffer = tft_->getFramebuffer();
            if (framebuffer && !digitAtlasReady && glyphW > 0 && clearH > 0 && frequencyRasterCache_.enabled()) {
                rasterKey = makeFrequencyRasterKey(
                    textBuf, freqColor, PALETTE_BG, static_cast<uint16_t>(fontSize), paletteRevision_, glyphLeft,
                    clearY, glyphW, clearH,
                    frequencyRasterFlags(muted, isPhotoRadar, isAlpOverride, band == BAND_LASER));
                rasterKeyValid = true;
            }
        }

        bool cachedRender = false;
        const bool numericDeltaEligible =
            digitAtlasReady && framebuffer && textChanged && elementCaches_.frequency.valid &&
            elementCaches_.frequency.lastUsedOfr && (elementCaches_.frequency.lastColor == freqColor) &&
            (elementCaches_.frequency.lastDrawX == x + glyphXMin) &&
            (elementCaches_.frequency.lastDrawWidth == glyphXMax - glyphXMin) &&
            DisplayFrequencyDigitAtlas::isNumericFrequencyText(elementCaches_.frequency.lastText);

        if (numericDeltaEligible) {
            int16_t dirtyX = 0;
            int16_t dirtyY = 0;
            int16_t dirtyW = 0;
            int16_t dirtyH = 0;
            if (frequencyDigitAtlas_.changedTextRect(elementCaches_.frequency.lastText, textBuf, dirtyX, dirtyY, dirtyW,
                                                     dirtyH)) {
                drawnRegion_.add(dirtyX, dirtyY, dirtyW, dirtyH, DisplayDirtyRegionSource::Frequency);
                FILL_RECT(dirtyX, dirtyY, dirtyW, dirtyH, PALETTE_BG);
                cachedRender = frequencyDigitAtlas_.restoreTextInRect(textBuf, freqColor, PALETTE_BG, framebuffer,
                                                                      CANVAS_WIDTH, dirtyX, dirtyY, dirtyW, dirtyH);
            }
        }

        if (!cachedRender) {
            if (clearH > 0 && clearW > 0) {
                drawnRegion_.add(static_cast<int16_t>(clearLeft), static_cast<int16_t>(clearY),
                                 static_cast<int16_t>(clearW), static_cast<int16_t>(clearH),
                                 DisplayDirtyRegionSource::Frequency);
                FILL_RECT(clearLeft, clearY, clearW, clearH, PALETTE_BG);
            }

            if (digitAtlasReady && framebuffer) {
                cachedRender =
                    frequencyDigitAtlas_.restoreText(textBuf, freqColor, PALETTE_BG, framebuffer, CANVAS_WIDTH);
            }
            if (!cachedRender && rasterKeyValid) {
                cachedRender = frequencyRasterCache_.restore(rasterKey, framebuffer, CANVAS_WIDTH);
            }
            if (!cachedRender) {
                // RGB565 → RGB888 via shared helper.
                const Rgb888 bg = rgb565ToRgb888(PALETTE_BG);
                const Rgb888 fg = rgb565ToRgb888(freqColor);
                fontMgr_.segment7.setBackgroundColor(bg.r, bg.g, bg.b);
                fontMgr_.segment7.setFontSize(fontSize);
                fontMgr_.segment7.setFontColor(fg.r, fg.g, fg.b);
                fontMgr_.segment7.setCursor(x, y);
                fontMgr_.segment7.printf("%s", textBuf);

                if (rasterKeyValid) {
                    frequencyRasterCache_.store(rasterKey, framebuffer, CANVAS_WIDTH);
                }
            }
        }

        elementCaches_.frequency.lastDrawX = x + glyphXMin;
        elementCaches_.frequency.lastDrawWidth = glyphXMax - glyphXMin;
    } else {
        // Fallback to software 7-segment renderer
        const float scale = DisplayLayout::FREQUENCY_FALLBACK_SCALE;
        SegMetrics m = segMetrics(scale);
        const int y = DisplayLayout::frequencyFallbackY(m.digitH);

        int width = measureSevenSegmentText(textBuf, scale);

        const int leftMargin = DisplayLayout::FREQUENCY_FALLBACK_LEFT_MARGIN;
        const int maxWidth = DisplayLayout::frequencyFallbackMaxWidth();
        int x = leftMargin + (maxWidth - width) / 2;
        if (x < leftMargin)
            x = leftMargin;

        if (isAlpOverride || band == BAND_LASER) {
            // Alpha text — use 14-segment renderer
            drawnRegion_.add(static_cast<int16_t>(x - 4), static_cast<int16_t>(y - 4), static_cast<int16_t>(width + 8),
                             static_cast<int16_t>(m.digitH + 8), DisplayDirtyRegionSource::Frequency);
            FILL_RECT(x - 4, y - 4, width + 8, m.digitH + 8, PALETTE_BG);
            draw14SegmentText(textBuf, x, y, scale, freqColor, PALETTE_BG);
        } else {
            drawnRegion_.add(static_cast<int16_t>(x - 2), static_cast<int16_t>(y), static_cast<int16_t>(width + 4),
                             static_cast<int16_t>(m.digitH + 4), DisplayDirtyRegionSource::Frequency);
            FILL_RECT(x - 2, y, width + 4, m.digitH + 4, PALETTE_BG);
            drawSevenSegmentText(textBuf, x, y, scale, freqColor, PALETTE_BG);
        }
    }

    strncpy(elementCaches_.frequency.lastText, textBuf, sizeof(elementCaches_.frequency.lastText));
    elementCaches_.frequency.lastText[sizeof(elementCaches_.frequency.lastText) - 1] = '\0';
    elementCaches_.frequency.lastColor = freqColor;
    elementCaches_.frequency.lastUsedOfr = usingOfr;
    elementCaches_.frequency.valid = true;
}

void V1Display::prewarmFrequencyDigitAtlas() {
    if (!fontMgr_.segment7Ready || !frequencyDigitAtlas_.enabled() || !tft_) {
        return;
    }

    uint16_t* framebuffer = tft_->getFramebuffer();
    if (!framebuffer) {
        return;
    }

    const int fontSize = DisplayLayout::FREQUENCY_OFR_FONT_SIZE;
    const int leftMargin = DisplayLayout::FREQUENCY_OFR_LEFT_MARGIN;
    const int y = DisplayLayout::frequencyOfrY();
    const int maxWidth = DisplayLayout::frequencyOfrMaxWidth();
    if (s_frequencyCachedNumericWidth <= 0) {
        s_frequencyCachedNumericWidth = DisplayFontManager::cachedTextWidth(
            fontMgr_.segment7, fontSize, "88.888", s_frequencyWidthCache, s_frequencyWidthCacheNextSlot);
    }

    int x = leftMargin + (maxWidth - s_frequencyCachedNumericWidth) / 2;
    if (x < leftMargin)
        x = leftMargin;

    constexpr const char* kReferencePrefixes[] = {
        "", "8", "88", "88.", "88.8", "88.88",
    };
    int offsets[DisplayFrequencyDigitAtlas::kTextPositions] = {};
    for (uint8_t pos = 1; pos < DisplayFrequencyDigitAtlas::kTextPositions; ++pos) {
        offsets[pos] = DisplayFontManager::cachedTextWidth(fontMgr_.segment7, fontSize, kReferencePrefixes[pos],
                                                           s_frequencyWidthCache, s_frequencyWidthCacheNextSlot);
    }

    constexpr int kCellPadPx = 3;
    int clearY = y - 8;
    int clearH = fontSize + 16;
    const int maxClearBottom = DisplayLayout::CONTENT_BOTTOM_Y;
    if (clearY + clearH > maxClearBottom)
        clearH = maxClearBottom - clearY;
    const int clearMinX = DisplayLayout::kFrequencyZoneRect.x;
    const int clearMaxX = DisplayLayout::kFrequencyZoneRect.x + DisplayLayout::kFrequencyZoneRect.w;

    const unsigned long warmStartMs = millis();
    const uint32_t storesBefore = frequencyDigitAtlas_.storeCount();

    fontMgr_.segment7.setBackgroundColor(0, 0, 0);
    fontMgr_.segment7.setFontColor(255, 255, 255);
    fontMgr_.segment7.setFontSize(fontSize);

    auto storeSymbol = [&](uint8_t pos, char symbol) {
        char symbolText[2] = {symbol, '\0'};
        int symbolXMin = 0;
        int symbolXMax = 0;
        getFrequencyTextBounds(fontMgr_, fontSize, symbolText, symbolXMin, symbolXMax);
        int cellLeft = x + offsets[pos] + symbolXMin - kCellPadPx;
        int cellRight = x + offsets[pos] + symbolXMax + kCellPadPx;
        cellLeft = std::max(cellLeft, clearMinX);
        cellRight = std::min(cellRight, clearMaxX);
        const int cellW = cellRight - cellLeft;
        if (cellW <= 0 || clearH <= 0) {
            return;
        }

        tft_->fillRect(cellLeft, clearY, cellW, clearH, PALETTE_BG);
        fontMgr_.segment7.setCursor(x + offsets[pos], y);
        fontMgr_.segment7.printf("%s", symbolText);
        frequencyDigitAtlas_.storeCell(pos, symbol, static_cast<int16_t>(cellLeft), static_cast<int16_t>(clearY),
                                       static_cast<int16_t>(cellW), static_cast<int16_t>(clearH), framebuffer,
                                       CANVAS_WIDTH, PALETTE_BG);
    };

    for (uint8_t pos = 0; pos < DisplayFrequencyDigitAtlas::kTextPositions; ++pos) {
        if (pos == 2) {
            storeSymbol(pos, '.');
            continue;
        }
        for (char digit = '0'; digit <= '9'; ++digit) {
            storeSymbol(pos, digit);
        }
    }

    elementCaches_.frequency.invalidate();
    drawnRegion_.reset();
    tft_->fillRect(DisplayLayout::kFrequencyZoneRect.x, DisplayLayout::kFrequencyZoneRect.y,
                   DisplayLayout::kFrequencyZoneRect.w, DisplayLayout::kFrequencyZoneRect.h, PALETTE_BG);

    Serial.printf("[DisplayCache] frequency digit atlas prewarm: ready=%d stores=%lu in %lu ms\n",
                  frequencyDigitAtlas_.ready() ? 1 : 0,
                  static_cast<unsigned long>(frequencyDigitAtlas_.storeCount() - storesBefore), millis() - warmStartMs);
}

void V1Display::prewarmFrequencyRasterCache() {
    if (!fontMgr_.segment7Ready || !frequencyRasterCache_.enabled() || !tft_ || !tft_->getFramebuffer()) {
        return;
    }

    struct FrequencyWarmSample {
        uint32_t freqMHz;
        Band band;
        bool muted;
        bool photo;
    };

    static constexpr FrequencyWarmSample kWarmSamples[] = {
        {0, BAND_NONE, false, false}, // --.---
        {0, BAND_LASER, false, false},
    };

    struct AlpWarmSample {
        const char* text;
        bool lidActive;
    };

    static constexpr AlpWarmSample kAlpWarmSamples[] = {
        {"truSPd", true},
        {"drgEYE", true},
        {"staLKr", false},
    };

    const bool savedAlpOverride = alpFreqOverride_;
    const bool savedAlpLid = alpLidActive_;
    char savedAlpText[sizeof(alpFreqText_)] = {};
    std::strncpy(savedAlpText, alpFreqText_, sizeof(savedAlpText));
    savedAlpText[sizeof(savedAlpText) - 1] = '\0';

    const unsigned long warmStartMs = millis();
    const uint32_t storesBefore = frequencyRasterCache_.storeCount();

    alpFreqOverride_ = false;
    alpLidActive_ = false;
    alpFreqText_[0] = '\0';
    for (const FrequencyWarmSample& sample : kWarmSamples) {
        elementCaches_.frequency.invalidate();
        drawFrequencySegment7(sample.freqMHz, sample.band, sample.muted, sample.photo);
    }

    for (const AlpWarmSample& sample : kAlpWarmSamples) {
        alpFreqOverride_ = true;
        alpLidActive_ = sample.lidActive;
        std::strncpy(alpFreqText_, sample.text, sizeof(alpFreqText_));
        alpFreqText_[sizeof(alpFreqText_) - 1] = '\0';
        elementCaches_.frequency.invalidate();
        drawFrequencySegment7(0, BAND_LASER, false, false);
    }

    alpFreqOverride_ = savedAlpOverride;
    alpLidActive_ = savedAlpLid;
    std::strncpy(alpFreqText_, savedAlpText, sizeof(alpFreqText_));
    alpFreqText_[sizeof(alpFreqText_) - 1] = '\0';

    elementCaches_.frequency.invalidate();
    drawnRegion_.reset();
    tft_->fillRect(DisplayLayout::kFrequencyZoneRect.x, DisplayLayout::kFrequencyZoneRect.y,
                   DisplayLayout::kFrequencyZoneRect.w, DisplayLayout::kFrequencyZoneRect.h, PALETTE_BG);

    Serial.printf("[DisplayCache] frequency raster prewarm: stores=%lu in %lu ms\n",
                  static_cast<unsigned long>(frequencyRasterCache_.storeCount() - storesBefore),
                  millis() - warmStartMs);
}

// --- Volume zero warning (flashing red text in frequency area) ---

void V1Display::drawVolumeZeroWarning() {
    // Flash at ~2Hz
    static unsigned long lastFlashTime = 0;
    static bool flashOn = true;
    unsigned long now = millis();
    if (now - lastFlashTime >= 250) {
        flashOn = !flashOn;
        lastFlashTime = now;
    }

    // Position warning centered in frequency area
    const int leftMargin = 120;
    const int rightMargin = 200;
    const int textScale = 6; // Large for visibility
    int maxWidth = SCREEN_WIDTH - leftMargin - rightMargin;
    int centerX = leftMargin + maxWidth / 2;
    int centerY = DisplayLayout::CONTENT_BOTTOM_Y / 2 + 10;

    // Use default built-in font (NULL) - has all ASCII characters
    // Each char is 6x8 pixels at scale 1, so "VOL 0" = 5 chars * 6 * scale wide
    const char* warningStr = "VOL 0";
    int charW = 6 * textScale;
    int charH = 8 * textScale;
    int textW = 5 * charW; // 5 characters
    int textX = centerX - textW / 2;
    int textY = centerY - charH / 2;

    // Clear the frequency area
    drawnRegion_.add(static_cast<int16_t>(leftMargin), static_cast<int16_t>(textY - 5), static_cast<int16_t>(maxWidth),
                     static_cast<int16_t>(charH + 10), DisplayDirtyRegionSource::Frequency);
    FILL_RECT(leftMargin, textY - 5, maxWidth, charH + 10, PALETTE_BG);

    // This path bypasses drawFrequency*(), so invalidate the normal frequency
    // render caches. When the warning expires, the next resting frame must
    // repaint the idle frequency area instead of cache-hitting against the
    // pre-warning value and leaving VOL 0 pixels in the framebuffer.
    elementCaches_.frequency.invalidate();

    if (flashOn) {
        tft_->setFont(NULL); // Default built-in font
        tft_->setTextSize(textScale);
        tft_->setTextColor(0xF800, PALETTE_BG); // Bright red on background
        tft_->setCursor(textX, textY);
        tft_->print(warningStr);
    }
}

// --- Frequency router ---

void V1Display::drawFrequency(uint32_t freqMHz, Band band, bool muted, bool isPhotoRadar) {
    drawFrequencySegment7(freqMHz, band, muted, isPhotoRadar);
}
