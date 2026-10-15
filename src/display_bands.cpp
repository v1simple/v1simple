#include "display.h"
#include "display_layout.h"
#include "display_draw.h"
#include "display_element_caches.h"
#include "display_palette.h"
#include "display_text.h"
#include "display_visual_contract.h"
#include "FreeSansBold24pt7b.h"
#include "settings.h"
#include "packet_parser.h"
#include <algorithm> // std::max
#include <cstring>

namespace {

constexpr int kBandLabelX = 82;
// A full-size "Ku" overlaps RSSI or card 0; keep K near its anchor with a compact suffix.
constexpr int kKuLabelX = 80;
constexpr int kKuSuffixX = 110;
constexpr int kKuSuffixVisualW = 18;
constexpr int kKuSuffixVisualH = 26;
constexpr int kKuNeighborGap = 2;
// FreeSansBold24's lowercase-u glyph, condensed horizontally from 23 to 18 px.
// drawBitmap rows are byte-aligned, hence three bytes per 18-pixel row.
constexpr uint8_t kKuSuffixBitmap[] PROGMEM = {
    0xFC, 0x07, 0xC0, 0xFC, 0x07, 0xC0, 0xFC, 0x07, 0xC0, 0xFC, 0x07, 0xC0,
    0xFC, 0x07, 0xC0, 0xFC, 0x07, 0xC0, 0xFC, 0x07, 0xC0, 0xFC, 0x07, 0xC0,
    0xFC, 0x07, 0xC0, 0xFC, 0x07, 0xC0, 0xFC, 0x07, 0xC0, 0xFC, 0x07, 0xC0,
    0xFC, 0x07, 0xC0, 0xFC, 0x07, 0xC0, 0xFC, 0x07, 0xC0, 0xFC, 0x07, 0xC0,
    0xFC, 0x07, 0xC0, 0xFC, 0x0F, 0xC0, 0xFE, 0x0F, 0xC0, 0xFF, 0x1F, 0xC0,
    0xFF, 0xFF, 0xC0, 0x7F, 0xFF, 0xC0, 0x7F, 0xF7, 0xC0, 0x3F, 0xF7, 0xC0,
    0x1F, 0xE7, 0xC0, 0x0F, 0xC0, 0x00,
};
static_assert(sizeof(kKuSuffixBitmap) == 3 * kKuSuffixVisualH, "compact Ku suffix bitmap size");
constexpr int kBandLabelTextSize = 1;
constexpr int kBandLabelSpacing = 43;
constexpr int kBandLabelStartY = 55;
constexpr int kBandLabelClearLeftPad = 4;
// FreeSansBold24 "Ka" extends about 56 px past the middle-left anchor after
// datum adjustment. Cover it and the compact Ku label while keeping the clear
// rectangle disjoint from the RSSI field at x=8..77.
constexpr int kBandLabelClearW = 65;
constexpr int kBandLabelClearH = 42;

} // namespace

bool V1Display::drawBandIndicators(uint8_t bandMask, bool muted, uint8_t bandFlashBits) {
    // Arrows and bands share one blink phase.
    updateBlinkPhase_();

    // Dim the physical cell at blink-off, retaining Ku identity instead of relabeling K.
    uint8_t effectiveBandMask = bandMask;
    if (!blinkPhase_) {
        effectiveBandMask &= ~bandFlashBits;
    }
    const bool kuBlinkOff = (bandMask & BAND_KU) != 0 && (bandFlashBits & BAND_K) != 0 && !blinkPhase_;

    if (elementCaches_.bands.valid && effectiveBandMask == elementCaches_.bands.lastMask &&
        bandFlashBits == elementCaches_.bands.lastFlashBits && muted == elementCaches_.bands.lastMuted &&
        kuBlinkOff == elementCaches_.bands.lastKuBlinkOff &&
        elementCaches_.bands.lastPaletteRevision == paletteRevision_) {
        return false;
    }

    const int x = kBandLabelX;
    const int textSize = kBandLabelTextSize;
    const int spacing = kBandLabelSpacing;
    const int startY = kBandLabelStartY;

    const V1Settings& s = settings_.get();
    struct BandCell {
        const char* label;
        uint8_t mask;
        uint16_t color;
    } cells[DisplayLayout::BAND_CELL_COUNT] = {{"L", BAND_LASER, s.colorBandL},
                                               {"Ka", BAND_KA, s.colorBandKa},
                                               {"K", BAND_K, s.colorBandK},
                                               {"X", BAND_X, s.colorBandX}};

    // The V1's band-display row has no dedicated Ku LED — Ku alerts light the
    // K LED on the V1 itself. The live renderer adds BAND_KU only when the
    // V1-selected priority row is Ku; secondary Ku alerts stay in their cards.
    // Persisted and preview callers may also pass an explicit Ku identity.
    const bool kuIdentity = (effectiveBandMask & BAND_KU) != 0;
    if (kuIdentity) {
        cells[2].label = DisplayVisualContract::bandCellLabel(effectiveBandMask, 2);
        cells[2].mask = DisplayVisualContract::bandCellMask(effectiveBandMask, 2);
    }

    TFT_CALL(setFont)(&FreeSansBold24pt7b);
    TFT_CALL(setTextSize)(textSize);
    GFX_setTextDatum(ML_DATUM);

    static bool s_bandBaselineInit = false;
    static int16_t s_bandBaselineAdjust = 0;
    if (!s_bandBaselineInit) {
        int16_t x1, y1;
        uint16_t w, h;
        TFT_CALL(getTextBounds)("Ka", 0, 0, &x1, &y1, &w, &h);
        s_bandBaselineAdjust = y1;
        s_bandBaselineInit = true;
    }

    constexpr int kBandCellCount = DisplayLayout::BAND_CELL_COUNT;
    const int16_t clearX = static_cast<int16_t>(x - kBandLabelClearLeftPad);
    const uint16_t clearW = kBandLabelClearW;
    int16_t clearY[kBandCellCount]{};
    bool cellChanged[kBandCellCount]{};

    auto cellIsActive = [&](uint8_t mask, int index) -> bool {
        // Ku has no dedicated V1 flash bit. Its label follows the physical K
        // bit's phase while retaining Ku identity during the dark half-cycle.
        if (index == 2 && (mask & BAND_KU) != 0 && (bandFlashBits & BAND_K) != 0 && !blinkPhase_) {
            return false;
        }
        return (mask & DisplayVisualContract::bandCellMask(mask, index)) != 0;
    };
    auto colorForCell = [&](uint8_t mask, bool cellMuted, int index) -> uint16_t {
        const bool active = cellIsActive(mask, index);
        return active ? (cellMuted ? PALETTE_MUTED_OR_PERSISTED : cells[index].color) : TFT_DARKGREY;
    };

    auto overlapsGpsBadge = [&](int16_t rectY) -> bool {
        const DisplayLayout::DisplayRect gpsRect = DisplayLayout::gpsBadgeRect();
        return clearX < gpsRect.x + gpsRect.w && clearX + static_cast<int16_t>(clearW) > gpsRect.x &&
               rectY < gpsRect.y + gpsRect.h && rectY + static_cast<int16_t>(kBandLabelClearH) > gpsRect.y;
    };

    // When any band is blinking, the off-phase clear can sit very close to a
    // still-active neighboring glyph (real K+flashing Ka is the observed case).
    // Repaint the whole band stack on actual blink-phase changes so a clear
    // never leaves an adjacent steady label partially erased in the framebuffer
    // before the live path's correctness full-flush pushes it to the panel.
    const bool forceFullStack = !elementCaches_.bands.valid || bandFlashBits != 0 ||
                                elementCaches_.bands.lastPaletteRevision != paletteRevision_;
    const uint8_t previousMask = elementCaches_.bands.lastMask;
    const bool previousMuted = elementCaches_.bands.lastMuted;
    bool anyCellChanged = false;
    bool gpsNeedsRestore = false;
    int16_t unionY = 0;
    uint16_t unionH = 0;
    for (int i = 0; i < kBandCellCount; ++i) {
        int labelY = startY + i * spacing;
        labelY += s_bandBaselineAdjust;
        clearY[i] = static_cast<int16_t>(labelY - kBandLabelClearH / 2);
        if (i == 0) {
            unionY = clearY[i];
            unionH = kBandLabelClearH;
        } else {
            int16_t maxY = static_cast<int16_t>(unionY + unionH);
            const int16_t newMaxY = static_cast<int16_t>(clearY[i] + kBandLabelClearH);
            if (clearY[i] < unionY)
                unionY = clearY[i];
            if (newMaxY > maxY)
                maxY = newMaxY;
            unionH = static_cast<uint16_t>(maxY - unionY);
        }

        const bool changed =
            forceFullStack ||
            colorForCell(previousMask, previousMuted, i) != colorForCell(effectiveBandMask, muted, i) ||
            strcmp(DisplayVisualContract::bandCellLabel(previousMask, i), cells[i].label) != 0;
        cellChanged[i] = changed;
        anyCellChanged = anyCellChanged || changed;
        gpsNeedsRestore = gpsNeedsRestore || (changed && overlapsGpsBadge(clearY[i]));
    }

    if (!anyCellChanged) {
        elementCaches_.bands.lastMask = effectiveBandMask;
        elementCaches_.bands.lastFlashBits = bandFlashBits;
        elementCaches_.bands.lastKuBlinkOff = kuBlinkOff;
        elementCaches_.bands.lastMuted = muted;
        elementCaches_.bands.lastPaletteRevision = paletteRevision_;
        elementCaches_.bands.valid = true;
        TFT_CALL(setFont)(NULL);
        TFT_CALL(setTextSize)(1);
        return false;
    }

    // Past the cache early-return: about to paint band cells in the left column.
    // Only the top band cell overlaps the GPS badge; avoid invalidating GPS for
    // lower-band-only updates so those independent item regions stay isolated.
    if (gpsNeedsRestore) {
        dirty_.gpsIndicator = true;
    }

    // The K/X clear windows reach into the top-left of card position 0.  When
    // that card is already on the framebuffer, subtract its rectangle from
    // band background clears; drawSecondaryAlertCards() may legitimately hit
    // its cache and skip repainting later in this frame.  With no drawn card,
    // retain the original full clear so L/Ka/K/Ku/X cleanup is unchanged.
    const DisplayLayout::DisplayRect card0Rect = DisplayLayout::cardRect(0);
    const bool protectCard0 = elementCaches_.cards.lastDrawnPositions[0].band != BAND_NONE;
    auto paintBandClear = [&](int16_t rectY, int16_t rectW, int16_t rectH) {
        auto paint = [&](int16_t px, int16_t py, int16_t pw, int16_t ph) {
            if (pw <= 0 || ph <= 0) {
                return;
            }
            drawnRegion_.add(px, py, pw, ph, DisplayDirtyRegionSource::Bands);
            FILL_RECT(px, py, pw, ph, PALETTE_BG);
        };

        const int16_t rectRight = static_cast<int16_t>(clearX + rectW);
        const int16_t rectBottom = static_cast<int16_t>(rectY + rectH);
        const int16_t cardRight = static_cast<int16_t>(card0Rect.x + card0Rect.w);
        const int16_t cardBottom = static_cast<int16_t>(card0Rect.y + card0Rect.h);
        const bool overlapsCard = protectCard0 && clearX < cardRight && rectRight > card0Rect.x && rectY < cardBottom &&
                                  rectBottom > card0Rect.y;
        if (!overlapsCard) {
            paint(clearX, rectY, rectW, rectH);
            return;
        }

        const int16_t overlapTop = std::max<int16_t>(rectY, card0Rect.y);
        const int16_t overlapBottom = std::min<int16_t>(rectBottom, cardBottom);
        paint(clearX, rectY, rectW, static_cast<int16_t>(overlapTop - rectY));
        paint(clearX, overlapTop, static_cast<int16_t>(card0Rect.x - clearX),
              static_cast<int16_t>(overlapBottom - overlapTop));
        paint(cardRight, overlapTop, static_cast<int16_t>(rectRight - cardRight),
              static_cast<int16_t>(overlapBottom - overlapTop));
        paint(clearX, overlapBottom, rectW, static_cast<int16_t>(rectBottom - overlapBottom));
    };

    if (forceFullStack) {
        paintBandClear(unionY, static_cast<int16_t>(clearW), static_cast<int16_t>(unionH));
    } else {
        for (int i = 0; i < kBandCellCount; ++i) {
            if (!cellChanged[i]) {
                continue;
            }
            paintBandClear(clearY[i], static_cast<int16_t>(clearW), static_cast<int16_t>(kBandLabelClearH));
        }
    }

    for (int i = 0; i < kBandCellCount; ++i) {
        if (!forceFullStack && !cellChanged[i]) {
            continue;
        }
        const bool isActive = cellIsActive(effectiveBandMask, i);
        int labelY = startY + i * spacing;
        labelY += s_bandBaselineAdjust;
        uint16_t col = isActive ? (muted ? PALETTE_MUTED_OR_PERSISTED : cells[i].color) : TFT_DARKGREY;
        // Draw foreground only. Arduino_GFX custom-font drawChar() fills the full
        // xAdvance × yAdvance advance box from the line top. FreeSansBold24's
        // yAdvance (~56 px) exceeds the 43 px cell pitch, so a two-arg
        // setTextColor(col, PALETTE_BG) here erased the top ~9 rows of the
        // next cell's glyph on every per-cell repaint. Full-stack repaints
        // self-healed (cells draw top-to-bottom), but dirty-path repaints of
        // a single cell clipped the unchanged letter below, as reproduced in
        // bench runs 8a599c91 and 3d3e4114. The FILL_RECT calls above already
        // provide the background for every repainted cell; the glyph itself
        // must not paint background pixels.
        TFT_CALL(setTextColor)(col);
        if (i == 2 && kuIdentity) {
            GFX_drawString(tft_, "K", kKuLabelX, labelY);
            TFT_CALL(drawBitmap)(kKuSuffixX, labelY - kKuSuffixVisualH / 2, kKuSuffixBitmap,
                                 kKuSuffixVisualW, kKuSuffixVisualH, col);
        } else {
            GFX_drawString(tft_, cells[i].label, x, labelY);
        }
    }

    elementCaches_.bands.lastMask = effectiveBandMask;
    elementCaches_.bands.lastFlashBits = bandFlashBits;
    elementCaches_.bands.lastKuBlinkOff = kuBlinkOff;
    elementCaches_.bands.lastMuted = muted;
    elementCaches_.bands.lastPaletteRevision = paletteRevision_;
    elementCaches_.bands.valid = true;

    TFT_CALL(setFont)(NULL);
    TFT_CALL(setTextSize)(1);
    return true;
}

// DisplayState::signalBars is the 0-8 LED count from ESP Spec 3.015 Table 9.1.
// This six-cell meter passes 0-6 through and saturates higher counts.

void V1Display::drawVerticalSignalBars(uint8_t frontStrength, uint8_t rearStrength, Band band, bool muted) {
    constexpr int barCount = DisplayLayout::MAIN_SIGNAL_BAR_COUNT;

    uint8_t strength = std::max(frontStrength, rearStrength);
    if (strength > barCount)
        strength = barCount;

    if (elementCaches_.bars.valid && strength == elementCaches_.bars.lastStrength &&
        muted == elementCaches_.bars.lastMuted && elementCaches_.bars.lastPaletteRevision == paletteRevision_) {
        return;
    }

    bool hasSignal = (strength > 0);

    const V1Settings& s = settings_.get();
    // Paint each stored segment color without interpolation.
    const uint16_t* barColors = s.colorBars;

    // The six-bar stack owns this lane; neighboring badges and cards must stay outside it.
    const DisplayLayout::DisplayRect stackRect = DisplayLayout::mainSignalBarsRect();
    // A palette-revision change repaints the full stack: lit-state deltas
    // can't see color changes, and lit bars must take their new colors.
    const bool paletteChanged =
        elementCaches_.bars.valid && elementCaches_.bars.lastPaletteRevision != paletteRevision_;
    const bool forceFullStack = !elementCaches_.bars.valid || paletteChanged;
    if (forceFullStack) {
        drawnRegion_.add(stackRect.x, stackRect.y, stackRect.w, stackRect.h, DisplayDirtyRegionSource::SignalBars);
    }
    int dirtyTop = SCREEN_HEIGHT;
    int dirtyBottom = 0;

    for (int i = 0; i < barCount; i++) {
        bool wasLit = elementCaches_.bars.valid && (i < elementCaches_.bars.lastStrength);
        bool isLit = hasSignal && (i < strength);
        bool wasMutedLit = elementCaches_.bars.valid && wasLit && elementCaches_.bars.lastMuted;
        bool isMutedLit = isLit && muted;

        if (!forceFullStack && wasLit == isLit && wasMutedLit == isMutedLit) {
            continue;
        }

        const DisplayLayout::DisplayRect barRect = DisplayLayout::mainSignalBarRect(i);
        if (!forceFullStack) {
            if (barRect.y < dirtyTop)
                dirtyTop = barRect.y;
            const int bottom = barRect.y + barRect.h;
            if (bottom > dirtyBottom)
                dirtyBottom = bottom;
        }

        uint16_t fillColor;
        if (!isLit) {
            fillColor = 0x1082;
        } else if (muted) {
            fillColor = PALETTE_MUTED;
        } else {
            fillColor = barColors[i];
        }

        FILL_ROUND_RECT(barRect.x, barRect.y, barRect.w, barRect.h, 2, fillColor);
    }

    if (!forceFullStack && dirtyTop < dirtyBottom) {
        // Adjacent changed bars share one DrawnRegion entry.
        drawnRegion_.add(stackRect.x, static_cast<int16_t>(dirtyTop), stackRect.w,
                         static_cast<int16_t>(dirtyBottom - dirtyTop), DisplayDirtyRegionSource::SignalBars);
    }

    elementCaches_.bars.lastStrength = strength;
    elementCaches_.bars.lastMuted = muted;
    elementCaches_.bars.lastPaletteRevision = paletteRevision_;
    elementCaches_.bars.valid = true;
}
