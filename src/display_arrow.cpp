/**
 * Direction Arrow Renderer — extracted from display.cpp (Phase 2I)
 *
 * Draws the stylised front / side / rear direction arrow triplet on the
 * right side of the display, with blink animation and per-arrow caching.
 */

#include "display.h"
#include "display_layout.h"
#include "display_draw.h"
#include "display_dirty_flags.h"
#include "display_element_caches.h"
#include "display_palette.h"
#include "settings.h"
#include "perf_metrics.h"
#include "packet_parser.h"                  // Direction enum, DIR_FRONT/SIDE/REAR
#include "modules/alp/alp_runtime_module.h" // AlpLaserDirection enum (for laser-color override)

// Blink timer moved to V1Display::updateBlinkPhase_() for lockstep sync with
// drawBandIndicators. See display.h BLINK_INTERVAL_MS / blinkPhase_ /
// updateBlinkPhase_() and the display module API notes for rationale.

// ============================================================================
// arrowBoundingRect — logical bounding rect of the entire arrow cluster
//
// Mirrors the clear-region math inside drawDirectionArrow (the full-redraw
// `FILL_RECT(clearLeft, totalTop, ...)` path). Exposed as a static helper so
// the live-update orchestrator can flushRegion() over exactly the pixels that
// drawDirectionArrow may have touched, without duplicating layout constants.
//
// Any change to arrow sizing (scale, cx/cy offset, topH/bottomH, sideBarH, gap)
// MUST be reflected here AND in drawDirectionArrow. The rect is a superset of
// both code paths (single-arrow clear + full cluster clear) so width/height
// over-reports by a few pixels are acceptable; under-reporting is not.
// ============================================================================
DisplayLayout::DisplayRect V1Display::arrowBoundingRect(bool raisedLayout) {
    return DisplayLayout::directionArrowClusterRect(raisedLayout);
}

// Draw large direction arrow (t4s3 style)
// flashBits indicates which arrows should blink (from image1 & ~image2)
void V1Display::drawDirectionArrow(Direction dir, bool muted, uint8_t flashBits, uint16_t frontColorOverride) {
    const bool forceFullRedraw = !elementCaches_.arrow.valid;

    // Advance the shared blink phase. drawBandIndicators calls this same
    // helper so arrows and band labels toggle in lockstep — they describe
    // the same underlying V1 signal via flashBits / bandFlashBits.
    updateBlinkPhase_();

    // Determine which arrows belong to this alert direction set.  showXxx is
    // independent of blink phase: an arrow that V1 reports as flashing is
    // still part of the current alert, so its cluster geometry never changes.
    const bool showFront = (dir & DIR_FRONT) != 0;
    const bool showSide = (dir & DIR_SIDE) != 0;
    const bool showRear = (dir & DIR_REAR) != 0;

    // Per-arrow blink-off-phase flag.  Keep the three visual states distinct:
    //   1. arrow not in V1's active Image1 direction set → dim resting glyph
    //   2. arrow in dir, blink ON phase                  → full activeCol
    //   3. arrow in dir, blink OFF phase                 → PALETTE_BG erase
    // The prior blink bug was #3 being indistinguishable from #1.  Do not
    // solve that by erasing #1 too; the simple-arrow widget intentionally keeps
    // resting arrows visible as layout/context chrome.
    const bool blinkOffFront = showFront && (flashBits & 0x20) && !blinkPhase_;
    const bool blinkOffSide = showSide && (flashBits & 0x40) && !blinkPhase_;
    const bool blinkOffRear = showRear && (flashBits & 0x80) && !blinkPhase_;

    // Stylized stacked arrows sized/positioned to match the real V1 display
    int cx = SCREEN_WIDTH - 70; // right anchor
    int cy = SCREEN_HEIGHT / 2; // vertically centered

    // Position arrows to fit ABOVE frequency display at bottom
    // With multi-alert always enabled, use raised layout as default
    const bool raisedLayout = dirty_.multiAlert;
    if (raisedLayout) {
        cy = 94; // Raised but allow full-size arrows
        cx -= 6;
    } else {
        cy = 104;
        cx -= 6;
    }

    // Use slightly smaller arrows to give profile indicator more room
    float scale = 0.98f;

    // Top arrow (FRONT): Taller triangle pointing up - matches V1 proportions
    // Wider/shallower angle to match V1 reference
    const int topW = (int)(125 * scale);     // Width at base
    const int topH = (int)(70 * scale);      // Height
    const int topNotchW = (int)(63 * scale); // Notch width at bottom
    const int topNotchH = (int)(8 * scale);  // Notch height

    // Bottom arrow (REAR): Shorter/squatter triangle pointing down
    const int bottomW = (int)(125 * scale); // Same width as top
    const int bottomH = (int)(30 * scale);  // Shorter height
    const int bottomNotchW = (int)(63 * scale);
    const int bottomNotchH = (int)(8 * scale);

    // Calculate positions for equal gaps between arrows
    const int sideBarH = (int)(22 * scale);
    const int gap = (int)(15 * scale); // gap between arrows

    // Top arrow center: above side arrow with gap
    int topArrowCenterY = cy - sideBarH / 2 - gap - topH / 2;
    // Bottom arrow center: below side arrow with gap
    int bottomArrowCenterY = cy + sideBarH / 2 + gap + bottomH / 2;

    const V1Settings& s = settingsManager.get();
    // Get individual arrow colors (use muted color if muted)
    uint16_t frontCol = muted ? PALETTE_MUTED_OR_PERSISTED : s.colorArrowFront;
    if (!muted && frontColorOverride != 0) {
        frontCol = frontColorOverride;
    }
    uint16_t sideCol = muted ? PALETTE_MUTED_OR_PERSISTED : s.colorArrowSide;
    uint16_t rearCol = muted ? PALETTE_MUTED_OR_PERSISTED : s.colorArrowRear;
    // Dim resting arrows are intentionally visible when their V1 direction bit
    // is not active.  Blink OFF is handled separately by blinkOffXxx and must
    // still erase to PALETTE_BG so the V1 Image1/Image2 blink is visible.
    uint16_t offCol = TFT_DARKGREY;

    // ── ALP laser-direction color override (Rev 15 classifier → arrow color) ──
    //
    // When an ALP laser event is live (set via setAlpLaserEvent()) AND the display isn't
    // muted, recolor the arrow that matches the classified direction to match
    // the ALP control-pad LED convention the driver already knows:
    //
    //     FRONT → RED   (matches ALP pad front-alert LED; threat already past
    //                     the countermeasure window — informational but critical)
    //     REAR  → YELLOW (matches ALP pad rear-alert LED; countermeasure window
    //                      exists, driver can react)
    //     UNKNOWN → leave palette defaults (PDC-Only, pre-classification, or
    //                  above-speed-limit Pro Mode noise — no classified signal)
    //
    // Valentine's Law note: this does not suppress the V1's own direction
    // arrows for radar-band alerts. This override only fires when alpHasLaserEvent_
    // is true — i.e. when the synthetic ALP laser AlertData is the one being
    // painted, or when V1 laser is being concurrently suppressed by the
    // ownsLaserDisplay() path. Radar-band arrows keep their settings-driven colors.
    if (!muted && alpHasLaserEvent_) {
        const AlpLaserDirection laserDir = alpLaserEvent_.direction;
        if (laserDir == AlpLaserDirection::FRONT) {
            frontCol = TFT_RED;
        } else if (laserDir == AlpLaserDirection::REAR) {
            rearCol = TFT_YELLOW;
        }
        // UNKNOWN falls through — no override, keep existing defaults.
    }

    const bool frontVisibilityChanged = elementCaches_.arrow.valid && (showFront != elementCaches_.arrow.showFront);
    const bool sideVisibilityChanged = elementCaches_.arrow.valid && (showSide != elementCaches_.arrow.showSide);
    const bool rearVisibilityChanged = elementCaches_.arrow.valid && (showRear != elementCaches_.arrow.showRear);
    if (frontVisibilityChanged || sideVisibilityChanged || rearVisibilityChanged) {
        // A direction-set change can promote a just-blinked-off active arrow
        // from PALETTE_BG back to the dim resting glyph.  That transition must
        // latch reliably on the panel; update(priority, ...) will use this
        // flag to avoid the flaky small-window partial path for this frame.
        arrowVisibilityForceFullFlush_ = true;
    }
    const bool blinkOffChanged = elementCaches_.arrow.valid && ((blinkOffFront != elementCaches_.arrow.blinkOffFront) ||
                                                                (blinkOffSide != elementCaches_.arrow.blinkOffSide) ||
                                                                (blinkOffRear != elementCaches_.arrow.blinkOffRear));
    const bool mutedChanged = elementCaches_.arrow.valid && (muted != elementCaches_.arrow.muted);
    const bool colorsChanged = elementCaches_.arrow.valid &&
                               ((frontCol != elementCaches_.arrow.frontCol) ||
                                (sideCol != elementCaches_.arrow.sideCol) || (rearCol != elementCaches_.arrow.rearCol));
    const bool layoutChanged = elementCaches_.arrow.valid && (raisedLayout != elementCaches_.arrow.raisedLayout);
    bool anyChanged = !elementCaches_.arrow.valid || frontVisibilityChanged || sideVisibilityChanged ||
                      rearVisibilityChanged || blinkOffChanged || mutedChanged || colorsChanged || layoutChanged;

    // If nothing changed, skip redraw entirely
    if (!anyChanged) {
        return;
    }
    arrowPaintedThisFrame_ = true;
    // Past the cache early-return: this call is going to paint pixels into the
    // arrow region. Contribute the cluster bounding rect to the per-frame
    // DrawnRegion union so update(priority, ...)'s dispatch can flushRegion()
    // exactly the pixels we touched.
    {
        const DisplayLayout::DisplayRect r = V1Display::arrowBoundingRect(raisedLayout);
        drawnRegion_.add(r.x, r.y, r.w, r.h, DisplayDirtyRegionSource::Arrows);
    }
    perfRecordDisplayRedrawReason(PerfDisplayRedrawReason::ArrowChange);

    // Calculate clear regions for each arrow
    const int maxW = (topW > bottomW) ? topW : bottomW;
    int clearLeft = cx - maxW / 2 - 10;
    int clearWidth = maxW + 20;
    int maxClearRight = SCREEN_WIDTH - 42;
    // D7 fix: only clip in centered layout. In raised layout we honour the
    // full glyph extent and restore the battery icon below — see memory note
    // display_arrow_partial_flush_clip_note_20260429.md and arrow_diag04.
    if (!raisedLayout && clearLeft + clearWidth > maxClearRight) {
        clearWidth = maxClearRight - clearLeft;
    }
    if (clearLeft + clearWidth > SCREEN_WIDTH) {
        clearWidth = SCREEN_WIDTH - clearLeft;
    }
    auto drawTriangleArrow = [&](int centerY, bool down, bool active, bool blinkOff, int triW, int triH, int notchW,
                                 int notchH, uint16_t activeCol, bool needsClear) {
        // Clear just this arrow's region if needed
        if (needsClear) {
            int arrowTop = centerY - triH / 2 - 2;
            int arrowHeight = triH + 4;
            FILL_RECT(clearLeft, arrowTop, clearWidth, arrowHeight, PALETTE_BG);
        }

        // Three-state fill: blinkOff erases the active flashing segment,
        // active draws bright, and inactive draws the dim resting glyph.
        uint16_t fillCol = blinkOff ? PALETTE_BG : (active ? activeCol : offCol);
        uint16_t outlineCol = TFT_BLACK; // Black outline like V1

        // Triangle points
        int tipX = cx;
        int tipY = centerY + (down ? triH / 2 : -triH / 2);
        int baseLeftX = cx - triW / 2;
        int baseRightX = cx + triW / 2;
        int baseY = centerY + (down ? -triH / 2 : triH / 2);

        // Fill the main triangle
        FILL_TRIANGLE(tipX, tipY, baseLeftX, baseY, baseRightX, baseY, fillCol);

        // Notch cutout at the base (opposite of tip)
        int notchY = down ? (baseY - notchH) : baseY;
        FILL_RECT(cx - notchW / 2, notchY, notchW, notchH, fillCol);

        // Draw outline - triangle edges
        DRAW_LINE(tipX, tipY, baseLeftX, baseY, outlineCol);
        DRAW_LINE(tipX, tipY, baseRightX, baseY, outlineCol);
        // Base line with notch gap
        DRAW_LINE(baseLeftX, baseY, cx - notchW / 2, baseY, outlineCol);
        DRAW_LINE(cx + notchW / 2, baseY, baseRightX, baseY, outlineCol);
        // Notch outline
        if (down) {
            DRAW_LINE(cx - notchW / 2, baseY, cx - notchW / 2, baseY - notchH, outlineCol);
            DRAW_LINE(cx - notchW / 2, baseY - notchH, cx + notchW / 2, baseY - notchH, outlineCol);
            DRAW_LINE(cx + notchW / 2, baseY - notchH, cx + notchW / 2, baseY, outlineCol);
        } else {
            DRAW_LINE(cx - notchW / 2, baseY, cx - notchW / 2, baseY + notchH, outlineCol);
            DRAW_LINE(cx - notchW / 2, baseY + notchH, cx + notchW / 2, baseY + notchH, outlineCol);
            DRAW_LINE(cx + notchW / 2, baseY + notchH, cx + notchW / 2, baseY, outlineCol);
        }
    };

    auto drawSideArrow = [&](bool active, bool blinkOff, bool needsClear) {
        // Clear just the side arrow region if needed
        if (needsClear) {
            [[maybe_unused]] const int headW = (int)(28 * scale);
            [[maybe_unused]] const int headH = (int)(22 * scale);
            int sideTop = cy - headH - 2;
            int sideHeight = headH * 2 + 4;
            FILL_RECT(clearLeft, sideTop, clearWidth, sideHeight, PALETTE_BG);
        }

        uint16_t fillCol = blinkOff ? PALETTE_BG : (active ? sideCol : offCol);
        uint16_t outlineCol = TFT_BLACK;    // Black outline like V1
        const int barW = (int)(66 * scale); // Center bar width
        const int barH = sideBarH;
        [[maybe_unused]] const int headW = (int)(28 * scale); // Arrow head width
        [[maybe_unused]] const int headH = (int)(22 * scale); // Arrow head height
        const int halfH = barH / 2;

        // Fill center bar
        FILL_RECT(cx - barW / 2, cy - halfH, barW, barH, fillCol);

        // Fill left arrow head
        FILL_TRIANGLE(cx - barW / 2 - headW, cy, cx - barW / 2, cy - headH, cx - barW / 2, cy + headH, fillCol);
        // Fill right arrow head
        FILL_TRIANGLE(cx + barW / 2 + headW, cy, cx + barW / 2, cy - headH, cx + barW / 2, cy + headH, fillCol);

        // Outline - top edge
        DRAW_LINE(cx - barW / 2, cy - halfH, cx + barW / 2, cy - halfH, outlineCol);
        // Outline - bottom edge
        DRAW_LINE(cx - barW / 2, cy + halfH, cx + barW / 2, cy + halfH, outlineCol);
        // Outline - left arrow head
        DRAW_LINE(cx - barW / 2, cy - headH, cx - barW / 2 - headW, cy, outlineCol);
        DRAW_LINE(cx - barW / 2 - headW, cy, cx - barW / 2, cy + headH, outlineCol);
        // Outline - right arrow head
        DRAW_LINE(cx + barW / 2, cy - headH, cx + barW / 2 + headW, cy, outlineCol);
        DRAW_LINE(cx + barW / 2 + headW, cy, cx + barW / 2, cy + headH, outlineCol);
    };

    // The three arrow glyphs are not vertically disjoint: the side-arrow clear
    // band overlaps both the front and rear base notches, and the triangle-base
    // clears clip the side arrow by a couple of pixels. A targeted single-arrow
    // repaint can therefore erase neighboring arrow bases without redrawing the
    // neighbor. Redraw the full cluster whenever any arrow state changes.
    [[maybe_unused]] const int headH = (int)(22 * scale);
    int totalTop = topArrowCenterY - topH / 2 - 2;
    int totalBottom = bottomArrowCenterY + bottomH / 2 + 2;
    FILL_RECT(clearLeft, totalTop, clearWidth, totalBottom - totalTop, PALETTE_BG);

    // Draw all three arrows
    drawTriangleArrow(topArrowCenterY, false, showFront, blinkOffFront, topW, topH, topNotchW, topNotchH, frontCol,
                      false);
    drawSideArrow(showSide, blinkOffSide, false);
    drawTriangleArrow(bottomArrowCenterY, true, showRear, blinkOffRear, bottomW, bottomH, bottomNotchW, bottomNotchH,
                      rearCol, false);

    // Update cache
    elementCaches_.arrow.showFront = showFront;
    elementCaches_.arrow.showSide = showSide;
    elementCaches_.arrow.showRear = showRear;
    elementCaches_.arrow.blinkOffFront = blinkOffFront;
    elementCaches_.arrow.blinkOffSide = blinkOffSide;
    elementCaches_.arrow.blinkOffRear = blinkOffRear;
    elementCaches_.arrow.muted = muted;
    elementCaches_.arrow.frontCol = frontCol;
    elementCaches_.arrow.sideCol = sideCol;
    elementCaches_.arrow.rearCol = rearCol;
    elementCaches_.arrow.raisedLayout = raisedLayout;
    elementCaches_.arrow.valid = true;

    // D7 fix: in raised layout the cluster clear extends past SCREEN_WIDTH-42
    // and overlaps the battery icon rect (SCREEN_WIDTH-50, 0, 48, 30). Mirror
    // the existing pattern in drawProfileIndicator: invalidate the icon cache
    // and redraw so the corner doesn't go blank for a frame. drawBatteryIndicator
    // is cache-gated for the percentage text, so this stays cheap on idle frames.
    if (raisedLayout) {
        elementCaches_.battery.iconValid = false;
        drawBatteryIndicator();
    }
}
