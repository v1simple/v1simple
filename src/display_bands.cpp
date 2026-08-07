/**
 * Band and signal-bar renderers.
 *
 * drawBandIndicators  — vertical L/Ka/K/X stack (left side)
 * drawBandBadge       — small rounded badge overlay
 * drawVerticalSignalBars — six-bar V1Simple projection of the V1 bitmap
 */

#include "display.h"
#include "display_layout.h"
#include "display_draw.h"
#include "display_element_caches.h"
#include "display_palette.h"
#include "display_text.h"
#include "display_visual_contract.h"
#include "FreeSansBold24pt7b.h"
#include "settings.h"
#include "perf_metrics.h"
#include "packet_parser.h"
#include <algorithm> // std::max
#include <cstring>

// Band labels and arrows share V1Display's blink phase.

namespace {

constexpr int kBandLabelX = 82;
constexpr int kBandLabelTextSize = 1;
constexpr int kBandLabelSpacing = 43;
constexpr int kBandLabelStartY = 55;
constexpr int kBandLabelClearLeftPad = 5;
// FreeSansBold24 "Ka" extends about 56 px past the middle-left anchor after
// datum adjustment. Include the full glyph in the partial-flush rectangle.
constexpr int kBandLabelClearW = 66;
constexpr int kBandLabelClearH = 42;

} // namespace

// ============================================================================
// Band indicator stack (vertical L / Ka / K / X on the left)
// ============================================================================
bool V1Display::drawBandIndicators(uint8_t bandMask, bool muted, uint8_t bandFlashBits) {
    // Advance the shared blink phase. drawDirectionArrow calls this same
    // helper so arrows and band labels toggle in lockstep — they describe
    // the same underlying V1 signal via flashBits / bandFlashBits.
    updateBlinkPhase_();

    // Apply blink: if flash bit is set and we're in OFF phase, treat band as inactive
    uint8_t effectiveBandMask = bandMask;
    if (!blinkPhase_) {
        effectiveBandMask &= ~bandFlashBits;
    }

    if (elementCaches_.bands.valid && effectiveBandMask == elementCaches_.bands.lastMask &&
        muted == elementCaches_.bands.lastMuted && elementCaches_.bands.lastPaletteRevision == paletteRevision_) {
        return false;
    }

    const int x = kBandLabelX;
    const int textSize = kBandLabelTextSize;
    const int spacing = kBandLabelSpacing;
    const int startY = kBandLabelStartY;

    const V1Settings& s = settingsManager.get();
    struct BandCell {
        const char* label;
        uint8_t mask;
        uint16_t color;
    } cells[DisplayLayout::BAND_CELL_COUNT] = {{"L", BAND_LASER, s.colorBandL},
                                               {"Ka", BAND_KA, s.colorBandKa},
                                               {"K", BAND_K, s.colorBandK},
                                               {"X", BAND_X, s.colorBandX}};

    // The V1's band-display row has no dedicated Ku LED — Ku alerts light
    // the K LED on the V1 itself.  When
    // the caller passes BAND_KU in the mask (either as a per-alert band or
    // via DisplayState::hasKuAlert OR'd into activeBands upstream), we light
    // the K cell and re-label it "Ku" so the user sees Ku is the active band.
    // Resting screens pass mask=0 so cell stays inactive and label stays "K".
    const bool kuActive = (effectiveBandMask & BAND_KU) != 0;
    if (kuActive) {
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

    auto colorForCell = [&](uint8_t mask, bool cellMuted, int index) -> uint16_t {
        const bool active = (mask & DisplayVisualContract::bandCellMask(mask, index)) != 0;
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
    perfRecordDisplayRedrawReason(PerfDisplayRedrawReason::BandSetChange);

    if (forceFullStack) {
        drawnRegion_.add(clearX, unionY, static_cast<int16_t>(clearW), static_cast<int16_t>(unionH),
                         DisplayDirtyRegionSource::Bands);
        FILL_RECT(clearX, unionY, clearW, unionH, PALETTE_BG);
    } else {
        for (int i = 0; i < kBandCellCount; ++i) {
            if (!cellChanged[i]) {
                continue;
            }
            drawnRegion_.add(clearX, clearY[i], static_cast<int16_t>(clearW), static_cast<int16_t>(kBandLabelClearH),
                             DisplayDirtyRegionSource::Bands);
            FILL_RECT(clearX, clearY[i], clearW, kBandLabelClearH, PALETTE_BG);
        }
    }

    for (int i = 0; i < kBandCellCount; ++i) {
        if (!forceFullStack && !cellChanged[i]) {
            continue;
        }
        const bool isActive = (effectiveBandMask & DisplayVisualContract::bandCellMask(effectiveBandMask, i)) != 0;
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
        GFX_drawString(tft_, cells[i].label, x, labelY);
    }

    elementCaches_.bands.lastMask = effectiveBandMask;
    elementCaches_.bands.lastMuted = muted;
    elementCaches_.bands.lastPaletteRevision = paletteRevision_;
    elementCaches_.bands.valid = true;

    TFT_CALL(setFont)(NULL);
    TFT_CALL(setTextSize)(1);
    return true;
}

// ============================================================================
// Signal bars render cache is in elementCaches_.bars
// ============================================================================

// ============================================================================
// Vertical signal bars (right side, 6-bar meter)
//
// The meter paints DisplayState::signalBars, the 0-8 LED count decoded from
// V1's InfDisplayData bar graph byte per ESP Spec 3.015 Table 9.1. That table
// is generation-neutral, so the count arrives at protocol scale and this is
// where V1Simple's six display slots become the ceiling: counts above six
// saturate the meter. Within 0-6 the meter is an exact passthrough.
//
// Colors: each of the six segments is individually addressable via
// V1Settings::colorBars. Nothing is interpolated at draw time.
// ============================================================================

void V1Display::drawVerticalSignalBars(uint8_t frontStrength, uint8_t rearStrength, Band band, bool muted) {
    constexpr int barCount = DisplayLayout::MAIN_SIGNAL_BAR_COUNT;

    uint8_t strength = std::max(frontStrength, rearStrength);
    if (strength > barCount)
        strength = barCount;

    if (elementCaches_.bars.valid && strength == elementCaches_.bars.lastStrength &&
        muted == elementCaches_.bars.lastMuted && elementCaches_.bars.lastPaletteRevision == paletteRevision_) {
        return;
    }
    perfRecordDisplayRedrawReason(PerfDisplayRedrawReason::SignalBarChange);

    bool hasSignal = (strength > 0);

    const V1Settings& s = settingsManager.get();
    // Each segment carries its own colour. There is no draw-time interpolation:
    // the six stored values are painted as-is.
    const uint16_t* barColors = s.colorBars;

    // Geometry: the bars own their DisplayLayout lane exclusively —
    // vertical neighbors never enter it (status badges end at x=420, secondary
    // cards at x=430, arrow cluster starts at x≈493, profile/battery at x≥499).
    // The restored stack spans y=18..152. 6×(14+10)−10 = 134.
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
        // Strength changes affect a contiguous run of bars.  Track that run as
        // one item-owned window so a large jump does not spend most of the
        // multi-rect budget on adjacent signal-bar cells and accidentally force
        // the frame back to a full-canvas push.
        drawnRegion_.add(stackRect.x, static_cast<int16_t>(dirtyTop), stackRect.w,
                         static_cast<int16_t>(dirtyBottom - dirtyTop), DisplayDirtyRegionSource::SignalBars);
    }

    elementCaches_.bars.lastStrength = strength;
    elementCaches_.bars.lastMuted = muted;
    elementCaches_.bars.lastPaletteRevision = paletteRevision_;
    elementCaches_.bars.valid = true;
}
