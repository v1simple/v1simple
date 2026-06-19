/**
 * Band / Signal-bar Renderers — extracted from display.cpp (Phase 2K)
 *
 * drawBandIndicators  — vertical L/Ka/K/X stack (left side)
 * drawBandBadge       — small rounded badge overlay
 * drawVerticalSignalBars — 6-bar signal meter (right side)
 */

#include "display.h"
#include "display_layout.h"
#include "display_draw.h"
#include "display_element_caches.h"
#include "display_palette.h"
#include "display_text.h"
#include "FreeSansBold24pt7b.h"
#include "settings.h"
#include "perf_metrics.h"
#include "packet_parser.h"
#include <algorithm>  // std::max
#include <cstring>

// Blink timer moved to V1Display::updateBlinkPhase_() for lockstep sync with
// drawDirectionArrow. See display.h BLINK_INTERVAL_MS / blinkPhase_ /
// updateBlinkPhase_().

namespace {

constexpr int kBandLabelX = 82;
constexpr int kBandLabelTextSize = 1;
constexpr int kBandLabelSpacing = 43;
constexpr int kBandLabelStartY = 55;
constexpr int kBandLabelClearLeftPad = 5;
// FreeSansBold24 "Ka" extends ~56 px to the right of the ML anchor after
// datum adjustment. The previous 50 px dirty window could leave glyph pixels
// outside the partial-flush rect, which shows up on-device as clipped band
// labels when adjacent K/Ka cells change independently.
constexpr int kBandLabelClearW = 66;
constexpr int kBandLabelClearH = 42;

}  // namespace

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

    if (elementCaches_.bands.valid && effectiveBandMask == elementCaches_.bands.lastMask && muted == elementCaches_.bands.lastMuted) {
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
    } cells[4] = {
        {"L",  BAND_LASER, s.colorBandL},
        {"Ka", BAND_KA,    s.colorBandKa},
        {"K",  BAND_K,     s.colorBandK},
        {"X",  BAND_X,     s.colorBandX}
    };

    // The V1's band-display row has no dedicated Ku LED — Ku alerts light
    // the K LED on the V1 itself.  When
    // the caller passes BAND_KU in the mask (either as a per-alert band or
    // via DisplayState::hasKuAlert OR'd into activeBands upstream), we light
    // the K cell and re-label it "Ku" so the user sees Ku is the active band.
    // Resting screens pass mask=0 → cell stays inactive and label stays "K".
    const bool kuActive = (effectiveBandMask & BAND_KU) != 0;
    if (kuActive) {
        cells[2].label = "Ku";
        cells[2].mask  = static_cast<uint8_t>(BAND_K | BAND_KU);
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

    constexpr int kBandCellCount = 4;
    const int16_t clearX = static_cast<int16_t>(x - kBandLabelClearLeftPad);
    const uint16_t clearW = kBandLabelClearW;
    int16_t clearY[kBandCellCount]{};
    bool cellChanged[kBandCellCount]{};

    auto maskForCell = [&](uint8_t mask, int index) -> uint8_t {
        if (index == 2 && (mask & BAND_KU) != 0) {
            return static_cast<uint8_t>(BAND_K | BAND_KU);
        }
        return (index == 0) ? BAND_LASER : ((index == 1) ? BAND_KA : ((index == 2) ? BAND_K : BAND_X));
    };

    auto colorForCell = [&](uint8_t mask, bool cellMuted, int index) -> uint16_t {
        const bool active = (mask & maskForCell(mask, index)) != 0;
        return active ? (cellMuted ? PALETTE_MUTED_OR_PERSISTED : cells[index].color) : TFT_DARKGREY;
    };

    auto labelForCell = [&](uint8_t mask, int index) -> const char* {
        if (index == 2 && (mask & BAND_KU) != 0) {
            return "Ku";
        }
        return (index == 0) ? "L" : ((index == 1) ? "Ka" : ((index == 2) ? "K" : "X"));
    };

    auto overlapsGpsBadge = [&](int16_t rectY) -> bool {
        constexpr int16_t gpsX = 120;
        constexpr int16_t gpsY = 5;
        constexpr int16_t gpsW = 46;
        constexpr int16_t gpsH = 26;
        return clearX < gpsX + gpsW &&
               clearX + static_cast<int16_t>(clearW) > gpsX &&
               rectY < gpsY + gpsH &&
               rectY + static_cast<int16_t>(kBandLabelClearH) > gpsY;
    };

    // When any band is blinking, the OFF-phase clear can sit very close to a
    // still-active neighboring glyph (real K+flashing Ka is the observed case).
    // Repaint the whole band stack on actual blink-phase changes so a clear
    // never leaves an adjacent steady label partially erased in the framebuffer
    // before the live path's correctness full-flush pushes it to the panel.
    const bool forceFullStack = !elementCaches_.bands.valid || bandFlashBits != 0;
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
            if (clearY[i] < unionY) unionY = clearY[i];
            if (newMaxY > maxY) maxY = newMaxY;
            unionH = static_cast<uint16_t>(maxY - unionY);
        }

        const bool changed = forceFullStack ||
                             colorForCell(previousMask, previousMuted, i) != colorForCell(effectiveBandMask, muted, i) ||
                             strcmp(labelForCell(previousMask, i), cells[i].label) != 0;
        cellChanged[i] = changed;
        anyCellChanged = anyCellChanged || changed;
        gpsNeedsRestore = gpsNeedsRestore || (changed && overlapsGpsBadge(clearY[i]));
    }

    if (!anyCellChanged) {
        elementCaches_.bands.lastMask = effectiveBandMask;
        elementCaches_.bands.lastMuted = muted;
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
        drawnRegion_.add(clearX,
                         unionY,
                         static_cast<int16_t>(clearW),
                         static_cast<int16_t>(unionH),
                         DisplayDirtyRegionSource::Bands);
        FILL_RECT(clearX, unionY, clearW, unionH, PALETTE_BG);
    } else {
        for (int i = 0; i < kBandCellCount; ++i) {
            if (!cellChanged[i]) {
                continue;
            }
            drawnRegion_.add(clearX,
                             clearY[i],
                             static_cast<int16_t>(clearW),
                             static_cast<int16_t>(kBandLabelClearH),
                         DisplayDirtyRegionSource::Bands);
            FILL_RECT(clearX, clearY[i], clearW, kBandLabelClearH, PALETTE_BG);
        }
    }

    for (int i = 0; i < kBandCellCount; ++i) {
        if (!forceFullStack && !cellChanged[i]) {
            continue;
        }
        const bool isActive = (effectiveBandMask & maskForCell(effectiveBandMask, i)) != 0;
        int labelY = startY + i * spacing;
        labelY += s_bandBaselineAdjust;
        uint16_t col = isActive ? (muted ? PALETTE_MUTED_OR_PERSISTED : cells[i].color) : TFT_DARKGREY;
        TFT_CALL(setTextColor)(col, PALETTE_BG);
        GFX_drawString(tft_, cells[i].label, x, labelY);
    }

    elementCaches_.bands.lastMask = effectiveBandMask;
    elementCaches_.bands.lastMuted = muted;
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
// ============================================================================
void V1Display::drawVerticalSignalBars(uint8_t frontStrength, uint8_t rearStrength, Band band, bool muted) {
    const int barCount = 6;

    uint8_t strength = std::max(frontStrength, rearStrength);
    if (strength > barCount) strength = barCount;

    if (elementCaches_.bars.valid && strength == elementCaches_.bars.lastStrength && muted == elementCaches_.bars.lastMuted) {
        return;
    }
    perfRecordDisplayRedrawReason(PerfDisplayRedrawReason::SignalBarChange);

    bool hasSignal = (strength > 0);

    const V1Settings& s = settingsManager.get();
    uint16_t barColors[6] = {
        s.colorBar1, s.colorBar2, s.colorBar3,
        s.colorBar4, s.colorBar5, s.colorBar6
    };

    const int barWidth = 44;
    const int barHeight = 14;
    const int barSpacing = 10;
    const int totalH = barCount * (barHeight + barSpacing) - barSpacing;

    int startX = SCREEN_WIDTH - 200;
    int startY = 18;
    if (startY < 8) startY = 8;
    const bool forceFullStack = !elementCaches_.bars.valid;
    if (forceFullStack) {
        drawnRegion_.add(static_cast<int16_t>(startX),
                         static_cast<int16_t>(startY),
                         static_cast<int16_t>(barWidth),
                         static_cast<int16_t>(totalH),
                         DisplayDirtyRegionSource::SignalBars);
    }
    int dirtyTop = SCREEN_HEIGHT;
    int dirtyBottom = 0;

    for (int i = 0; i < barCount; i++) {
        bool wasLit = elementCaches_.bars.valid && (i < elementCaches_.bars.lastStrength);
        bool isLit = hasSignal && (i < strength);
        bool wasMutedLit = elementCaches_.bars.valid && wasLit && elementCaches_.bars.lastMuted;
        bool isMutedLit = isLit && muted;

        if (elementCaches_.bars.valid && wasLit == isLit && wasMutedLit == isMutedLit) {
            continue;
        }

        int visualIndex = barCount - 1 - i;
        int y = startY + visualIndex * (barHeight + barSpacing);
        if (!forceFullStack) {
            if (y < dirtyTop) dirtyTop = y;
            const int bottom = y + barHeight;
            if (bottom > dirtyBottom) dirtyBottom = bottom;
        }

        uint16_t fillColor;
        if (!isLit) {
            fillColor = 0x1082;
        } else if (muted) {
            fillColor = PALETTE_MUTED;
        } else {
            fillColor = barColors[i];
        }

        FILL_ROUND_RECT(startX, y, barWidth, barHeight, 2, fillColor);
    }

    if (!forceFullStack && dirtyTop < dirtyBottom) {
        // Strength changes affect a contiguous run of bars.  Track that run as
        // one item-owned window so a large jump does not spend most of the
        // multi-rect budget on adjacent signal-bar cells and accidentally force
        // the frame back to a full-canvas push.
        drawnRegion_.add(static_cast<int16_t>(startX),
                         static_cast<int16_t>(dirtyTop),
                         static_cast<int16_t>(barWidth),
                         static_cast<int16_t>(dirtyBottom - dirtyTop),
                         DisplayDirtyRegionSource::SignalBars);
    }

    elementCaches_.bars.lastStrength = strength;
    elementCaches_.bars.lastMuted = muted;
    elementCaches_.bars.valid = true;
}
