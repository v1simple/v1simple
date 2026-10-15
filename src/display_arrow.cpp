#include "display.h"
#include "display_layout.h"
#include "display_draw.h"
#include "display_dirty_flags.h"
#include "display_element_caches.h"
#include "display_palette.h"
#include "settings.h"
#include "packet_parser.h"                  // Direction enum, DIR_FRONT/SIDE/REAR
#include "modules/alp/alp_runtime_module.h" // AlpLaserDirection enum (for laser-color override)

// Must cover every arrow draw and clear path; under-reporting leaves stale pixels.
DisplayLayout::DisplayRect V1Display::arrowBoundingRect(bool raisedLayout) {
    return DisplayLayout::directionArrowClusterRect(raisedLayout);
}

// flashBits = image1 & ~image2, using the V1's physical direction bits.
void V1Display::drawDirectionArrow(Direction dir, bool muted, uint8_t flashBits, uint16_t frontColorOverride) {
    // Arrows and band labels share one blink phase.
    updateBlinkPhase_();

    // Active directions keep their geometry through the blink-off phase.
    const bool showFront = (dir & DIR_FRONT) != 0;
    const bool showRear = (dir & DIR_REAR) != 0;
    const bool showSide = (dir & DIR_SIDE) != 0;

    // Inactive arrows stay dim; only active flashing arrows erase during blink-off.
    const bool blinkOffFront = showFront && (flashBits & 0x20) && !blinkPhase_;
    const bool blinkOffSide = showSide && (flashBits & 0x40) && !blinkPhase_;
    const bool blinkOffRear = showRear && (flashBits & 0x80) && !blinkPhase_;

    int cx = SCREEN_WIDTH - 70; // right anchor
    int cy = SCREEN_HEIGHT / 2; // vertically centered

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

    // Match the V1's taller front arrow and shorter rear arrow.
    const int topW = (int)(125 * scale);     // Width at base
    const int topH = (int)(70 * scale);      // Height
    const int topNotchW = (int)(63 * scale); // Notch width at bottom
    const int topNotchH = (int)(8 * scale);  // Notch height

    const int bottomW = (int)(125 * scale); // Same width as top
    const int bottomH = (int)(30 * scale);  // Shorter height
    const int bottomNotchW = (int)(63 * scale);
    const int bottomNotchH = (int)(8 * scale);

    const int sideBarH = (int)(22 * scale);
    const int gap = (int)(15 * scale); // gap between arrows

    int topArrowCenterY = cy - sideBarH / 2 - gap - topH / 2;
    int bottomArrowCenterY = cy + sideBarH / 2 + gap + bottomH / 2;

    const V1Settings& s = settings_.get();
    uint16_t frontCol = muted ? PALETTE_MUTED_OR_PERSISTED : s.colorArrowFront;
    if (!muted && frontColorOverride != 0) {
        frontCol = frontColorOverride;
    }
    uint16_t sideCol = muted ? PALETTE_MUTED_OR_PERSISTED : s.colorArrowSide;
    uint16_t rearCol = muted ? PALETTE_MUTED_OR_PERSISTED : s.colorArrowRear;
    uint16_t offCol = TFT_DARKGREY;

    // Match ALP pad colors only on an ALP-owned laser frame; unknown direction
    // keeps the palette. V1 radar arrows retain their own direction and colors
    // (docs/VALENTINE_PHILOSOPHY.md, principle #2).
    if (!muted && alpHasLaserEvent_) {
        const AlpLaserDirection laserDir = alpLaserEvent_.direction;
        if (laserDir == AlpLaserDirection::FRONT) {
            frontCol = TFT_RED;
        } else if (laserDir == AlpLaserDirection::REAR) {
            rearCol = TFT_YELLOW;
        }
    }

    const bool frontVisibilityChanged = elementCaches_.arrow.valid && (showFront != elementCaches_.arrow.showFront);
    const bool sideVisibilityChanged = elementCaches_.arrow.valid && (showSide != elementCaches_.arrow.showSide);
    const bool rearVisibilityChanged = elementCaches_.arrow.valid && (showRear != elementCaches_.arrow.showRear);
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

    if (!anyChanged) {
        return;
    }
    // Record paint only after the cache check so unchanged frames need no transfer.
    {
        const DisplayLayout::DisplayRect r = V1Display::arrowBoundingRect(raisedLayout);
        drawnRegion_.add(r.x, r.y, r.w, r.h);
    }

    const int maxW = (topW > bottomW) ? topW : bottomW;
    int clearLeft = cx - maxW / 2 - 10;
    int clearWidth = maxW + 20;
    int maxClearRight = SCREEN_WIDTH - 42;
    // Only clip in centered layout. In raised layout we honour the full glyph
    // extent and restore the battery icon below.
    if (!raisedLayout && clearLeft + clearWidth > maxClearRight) {
        clearWidth = maxClearRight - clearLeft;
    }
    if (clearLeft + clearWidth > SCREEN_WIDTH) {
        clearWidth = SCREEN_WIDTH - clearLeft;
    }
    auto drawTriangleArrow = [&](int centerY, bool down, bool active, bool blinkOff, int triW, int triH, int notchW,
                                 int notchH, uint16_t activeCol) {
        uint16_t fillCol = blinkOff ? PALETTE_BG : (active ? activeCol : offCol);
        uint16_t outlineCol = TFT_BLACK; // Black outline like V1

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

    auto drawSideArrow = [&](bool active, bool blinkOff) {
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
    drawTriangleArrow(topArrowCenterY, false, showFront, blinkOffFront, topW, topH, topNotchW, topNotchH, frontCol);
    drawSideArrow(showSide, blinkOffSide);
    drawTriangleArrow(bottomArrowCenterY, true, showRear, blinkOffRear, bottomW, bottomH, bottomNotchW, bottomNotchH,
                      rearCol);

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

    // The raised cluster overlaps the battery rectangle. Invalidate and redraw
    // the icon after the arrow clear so the corner is restored in the same frame.
    if (raisedLayout) {
        elementCaches_.battery.iconValid = false;
        drawBatteryIndicator();
    }
}
