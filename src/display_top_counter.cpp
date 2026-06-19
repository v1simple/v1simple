/**
 * Top Counter / Bogey Counter rendering — extracted from display.cpp (Phase 2O)
 *
 * Contains 7-segment and 14-segment digit renderers, the top counter
 * (bogey counter) composites, and the mute badge.
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
#include <cstring>

using namespace DisplaySegments;

using DisplayLayout::TOP_COUNTER_FONT_SIZE;
using DisplayLayout::TOP_COUNTER_FIELD_X;
using DisplayLayout::TOP_COUNTER_FIELD_Y;
using DisplayLayout::TOP_COUNTER_FIELD_W;
using DisplayLayout::TOP_COUNTER_FIELD_H;
using DisplayLayout::TOP_COUNTER_TEXT_Y;
using DisplayLayout::TOP_COUNTER_PAD_RIGHT;
using DisplayLayout::TOP_COUNTER_FALLBACK_WIDTH;

// Top counter render cache is in elementCaches_.topCounter

namespace {

bool hasTrailingTopCounterSymbol(char symbol, bool showDot) {
    return (symbol != '\0' && symbol != ' ') || showDot;
}

bool isTopCounterDigit(char symbol) {
    return symbol >= '0' && symbol <= '9';
}

void buildTopCounterText(char primary,
                         bool primaryDot,
                         char secondary,
                         bool secondaryDot,
                         bool drawFixedPrimaryDot,
                         char (&buf)[8]) {
    size_t pos = 0;
    buf[pos++] = (primary == '\0') ? ' ' : primary;
    if (primaryDot && !drawFixedPrimaryDot) {
        buf[pos++] = '.';
    }
    if (hasTrailingTopCounterSymbol(secondary, secondaryDot)) {
        buf[pos++] = (secondary == '\0') ? ' ' : secondary;
        if (secondaryDot) {
            buf[pos++] = '.';
        }
    }
    buf[pos] = '\0';
}

}  // namespace

// --- 7-segment digit rendering ---

void V1Display::drawSevenSegmentDigit(int x, int y, float scale, char c, bool addDot, uint16_t onColor, uint16_t offColor) {
    SegMetrics m = segMetrics(scale);
    bool segments[7] = {false, false, false, false, false, false, false};
    // Segment layout:   0=top, 1=upper-right, 2=lower-right, 3=bottom, 4=lower-left, 5=upper-left, 6=middle

    if (c >= '0' && c <= '9') {
        for (int i = 0; i < 7; ++i) {
            segments[i] = DIGIT_SEGMENTS[c - '0'][i];
        }
    } else if (c == '-') {
        segments[6] = true; // Middle bar only
    } else if (c == '=' || c == '#') {
        // Three horizontal bars for laser alert (top, middle, bottom)
        // '#' is the decoded byte value for laser (73)
        segments[0] = segments[6] = segments[3] = true;
    } else if (c == 'A' || c == 'a') {
        // A = all but bottom segment
        segments[0] = segments[1] = segments[2] = segments[4] = segments[5] = segments[6] = true;
    } else if (c == 'L') {
        // Full L: bottom + lower-left + upper-left
        segments[3] = segments[4] = segments[5] = true;
    } else if (c == 'l' || c == '&') {
        // Logic (lowercase) L: bottom + lower-left only
        // '&' is used in decoder for little L (logic mode, byte value 24)
        segments[3] = segments[4] = true;
    } else if (c == 'S' || c == 's') {
        // S = top, upper-left, middle, lower-right, bottom (like 5)
        segments[0] = segments[5] = segments[6] = segments[2] = segments[3] = true;
    } else if (c == 'E') {
        // E = top, upper-left, middle, lower-left, bottom
        segments[0] = segments[5] = segments[6] = segments[4] = segments[3] = true;
    } else if (c == 'R') {
        // R = top, upper-left, middle, lower-left (fuller capital R)
        segments[0] = segments[5] = segments[6] = segments[4] = true;
    } else if (c == 'r') {
        // r = middle, lower-left (lowercase r style)
        segments[6] = segments[4] = true;
    } else if (c == 'J') {
        // J = Junk: upper-right, lower-right, bottom, lower-left
        segments[1] = segments[2] = segments[3] = segments[4] = true;
    } else if (c == 'P') {
        // P = Photo radar: top, upper-right, upper-left, middle, lower-left
        segments[0] = segments[1] = segments[5] = segments[6] = segments[4] = true;
    } else if (c == 'F') {
        // F = top, upper-left, middle, lower-left
        segments[0] = segments[5] = segments[6] = segments[4] = true;
    } else if (c == 'C') {
        // C = top, upper-left, lower-left, bottom
        segments[0] = segments[5] = segments[4] = segments[3] = true;
    } else if (c == 'U') {
        // U = upper-right, lower-right, bottom, lower-left, upper-left
        segments[1] = segments[2] = segments[3] = segments[4] = segments[5] = true;
    } else if (c == 'u') {
        // lowercase u = lower-right, bottom, lower-left
        segments[2] = segments[3] = segments[4] = true;
    } else if (c == 'b') {
        // b = upper-left, lower-left, bottom, lower-right, middle
        segments[5] = segments[4] = segments[3] = segments[2] = segments[6] = true;
    } else if (c == 'c') {
        // lowercase c = middle, lower-left, bottom
        segments[6] = segments[4] = segments[3] = true;
    } else if (c == 'd') {
        // d = upper-right, lower-right, bottom, lower-left, middle
        segments[1] = segments[2] = segments[3] = segments[4] = segments[6] = true;
    } else if (c == 'e') {
        // e = top, upper-left, lower-left, bottom, middle, upper-right (differs from capital E by having bottom right)
        segments[0] = segments[5] = segments[4] = segments[3] = segments[6] = true;
    }

    auto drawSeg = [&](int sx, int sy, int w, int h, bool on) {
        uint16_t col = on ? onColor : offColor;
        if (!on && offColor == PALETTE_BG) return;
        FILL_ROUND_RECT(sx, sy, w, h, scale, col);
    };

    int ax = x + m.segThick;
    int ay = y;
    int bx = x + m.segLen + m.segThick;
    int byTop = y + m.segThick;
    int byBottom = y + m.segLen + 2 * m.segThick;
    int dx = ax;
    int dy = y + 2 * m.segLen + 2 * m.segThick;
    int gx = ax;
    int gy = y + m.segLen + m.segThick;

    drawSeg(ax, ay, m.segLen, m.segThick, segments[0]);       // Top
    drawSeg(bx, byTop, m.segThick, m.segLen, segments[1]);    // Upper right
    drawSeg(bx, byBottom, m.segThick, m.segLen, segments[2]); // Lower right
    drawSeg(dx, dy, m.segLen, m.segThick, segments[3]);       // Bottom
    drawSeg(x, byBottom, m.segThick, m.segLen, segments[4]);  // Lower left
    drawSeg(x, byTop, m.segThick, m.segLen, segments[5]);     // Upper left
    drawSeg(gx, gy, m.segLen, m.segThick, segments[6]);       // Middle

    if (addDot) {
        int dotR = m.dot / 2 + 1;
        int dotX = x + m.digitW + dotR;
        int dotY = y + m.digitH - dotR;
        FILL_CIRCLE(dotX, dotY, dotR, onColor);
    }
}

// --- 7-segment text helpers ---

int V1Display::measureSevenSegmentText(const char* text, float scale) const {
    SegMetrics m = segMetrics(scale);
    int width = 0;
    size_t len = strlen(text);
    for (size_t i = 0; i < len; ++i) {
        if (text[i] == '.') continue;
        bool hasDot = (i + 1 < len && text[i + 1] == '.');
        width += m.digitW + m.spacing + (hasDot ? m.dot / 2 : 0);
        if (hasDot) ++i;
    }
    if (width > 0) {
        width -= m.spacing; // remove trailing spacing
    }
    return width;
}

int V1Display::drawSevenSegmentText(const char* text, int x, int y, float scale, uint16_t onColor, uint16_t offColor) {
    SegMetrics m = segMetrics(scale);
    int cursor = x;
    size_t len = strlen(text);
    for (size_t i = 0; i < len; ++i) {
        char c = text[i];
        if (c == '.') continue; // handled alongside previous digit
        bool hasDot = (i + 1 < len && text[i + 1] == '.');
        drawSevenSegmentDigit(cursor, y, scale, c, hasDot, onColor, offColor);
        cursor += m.digitW + m.spacing + (hasDot ? m.dot / 2 : 0);
        if (hasDot) ++i;
    }
    return cursor - x - m.spacing;
}

// --- 14-segment digit and text rendering ---

void V1Display::draw14SegmentDigit(int x, int y, float scale, char c, bool addDot, uint16_t onColor, uint16_t offColor) {
    SegMetrics m = segMetrics(scale);
    uint16_t pattern = get14SegPattern(c);

    auto drawHSeg = [&](int sx, int sy, int w, bool on) {
        uint16_t col = on ? onColor : offColor;
        if (!on && offColor == PALETTE_BG) return;
        FILL_ROUND_RECT(sx, sy, w, m.segThick, scale, col);
    };

    auto drawVSeg = [&](int sx, int sy, int h, bool on) {
        uint16_t col = on ? onColor : offColor;
        if (!on && offColor == PALETTE_BG) return;
        FILL_ROUND_RECT(sx, sy, m.segThick, h, scale, col);
    };

    auto drawDiag = [&](int x1, int y1, int x2, int y2, bool on) {
        uint16_t col = on ? onColor : offColor;
        if (!on && offColor == PALETTE_BG) return;
        // Draw thick diagonal line
        for (int t = -m.segThick/2; t <= m.segThick/2; t++) {
            DRAW_LINE(x1+t, y1, x2+t, y2, col);
            DRAW_LINE(x1, y1+t, x2, y2+t, col);
        }
    };

    int halfW = m.segLen / 2;
    int centerX = x + m.segThick + halfW;
    int midY = y + m.segLen + m.segThick;

    // Horizontal segments
    drawHSeg(x + m.segThick, y, m.segLen, pattern & S14_TOP);                           // Top
    drawHSeg(x + m.segThick, y + 2*m.segLen + 2*m.segThick, m.segLen, pattern & S14_BOT); // Bottom
    drawHSeg(x + m.segThick, midY, halfW - m.segThick/2, pattern & S14_ML);              // Middle-left
    drawHSeg(centerX + m.segThick/2, midY, halfW - m.segThick/2, pattern & S14_MR);      // Middle-right

    // Vertical segments - outer
    drawVSeg(x, y + m.segThick, m.segLen, pattern & S14_TL);                             // Top-left
    drawVSeg(x, y + m.segLen + 2*m.segThick, m.segLen, pattern & S14_BL);                // Bottom-left
    drawVSeg(x + m.segLen + m.segThick, y + m.segThick, m.segLen, pattern & S14_TR);     // Top-right
    drawVSeg(x + m.segLen + m.segThick, y + m.segLen + 2*m.segThick, m.segLen, pattern & S14_BR); // Bottom-right

    // Center vertical segments
    drawVSeg(centerX, y + m.segThick, m.segLen - m.segThick, pattern & S14_CT);          // Center-top
    drawVSeg(centerX, midY + m.segThick, m.segLen - m.segThick, pattern & S14_CB);       // Center-bottom

    // Diagonal segments
    int diagInset = m.segThick;
    drawDiag(x + diagInset, y + m.segThick + diagInset,
             centerX - diagInset, midY - diagInset, pattern & S14_DTL);                   // Diag top-left
    drawDiag(centerX + diagInset, y + m.segThick + diagInset,
             x + m.segLen + m.segThick - diagInset, midY - diagInset, pattern & S14_DTR); // Diag top-right
    drawDiag(x + diagInset, y + 2*m.segLen + m.segThick - diagInset,
             centerX - diagInset, midY + m.segThick + diagInset, pattern & S14_DBL);      // Diag bottom-left
    drawDiag(centerX + diagInset, midY + m.segThick + diagInset,
             x + m.segLen + m.segThick - diagInset, y + 2*m.segLen + m.segThick - diagInset, pattern & S14_DBR); // Diag bottom-right

    if (addDot) {
        int dotR = m.dot / 2 + 1;
        int dotX = x + m.digitW + dotR;
        int dotY = y + m.digitH - dotR;
        FILL_CIRCLE(dotX, dotY, dotR, onColor);
    }
}

int V1Display::draw14SegmentText(const char* text, int x, int y, float scale, uint16_t onColor, uint16_t offColor) {
    SegMetrics m = segMetrics(scale);
    int cursor = x;
    size_t len = strlen(text);
    for (size_t i = 0; i < len; ++i) {
        char c = text[i];
        if (c == '.') continue;
        bool hasDot = (i + 1 < len && text[i + 1] == '.');
        draw14SegmentDigit(cursor, y, scale, c, hasDot, onColor, offColor);
        cursor += m.digitW + m.spacing + (hasDot ? m.dot / 2 : 0);
        if (hasDot) ++i;
    }
    return cursor - x - m.spacing;
}

// --- Top counter (bogey counter) — Segment7 style ---

// Segment7 bogey counter (original V1 style).
// Uses Segment7 TTF font if available, falls back to software renderer.
void V1Display::drawTopCounterPair(char primary,
                                   bool muted,
                                   bool primaryDot,
                                   char secondary,
                                   bool secondaryDot) {
    const V1Settings& s = settingsManager.get();
    const bool hasSecondary = hasTrailingTopCounterSymbol(secondary, secondaryDot);
    const bool primaryIsDigit = isTopCounterDigit(primary);
    const bool secondaryIsDigit = !hasSecondary || isTopCounterDigit(secondary);
    const bool drawFixedPrimaryDot = !hasSecondary && primaryIsDigit && primaryDot;
    char buf[8] = {0};
    buildTopCounterText(primary, primaryDot, secondary, secondaryDot, drawFixedPrimaryDot, buf);

    // Check if color setting changed
    bool colorChanged = (s.colorBogey != elementCaches_.topCounter.lastBogeyColor);

    // Skip redraw if nothing changed
    if (elementCaches_.topCounter.counterValid && !colorChanged &&
        muted == elementCaches_.topCounter.lastMuted &&
        strcmp(buf, elementCaches_.topCounter.lastText) == 0) {
        return;
    }
    // Drawn-region tracking: own pixels from one column left of the field through
    // the 6-px gap that sits between us and the band-label clear column (which
    // starts at x=77 — see drawBandIndicators). Nothing else paints that gap, so
    // any residue there persists indefinitely; widening here makes the per-frame
    // FILL_RECT below absorb it.
    constexpr int kBandsColumnLeftX = 77;  // mirrors unionX in drawBandIndicators (x=82, x-5=77)
    constexpr int kClearLeftPad = 1;
    const int clearX = TOP_COUNTER_FIELD_X - kClearLeftPad;
    const int clearW = (kBandsColumnLeftX - clearX);
    drawnRegion_.add(static_cast<int16_t>(clearX),
                     DisplayLayout::kTopCounterRect.y,
                     static_cast<int16_t>(clearW),
                     DisplayLayout::kTopCounterRect.h,
                         DisplayDirtyRegionSource::Status);
    perfRecordDisplayRedrawReason(PerfDisplayRedrawReason::BogeyCounterChange);
    elementCaches_.topCounter.counterValid   = true;
    elementCaches_.topCounter.lastMuted      = muted;
    elementCaches_.topCounter.lastBogeyColor = s.colorBogey;
    strncpy(elementCaches_.topCounter.lastText, buf, sizeof(elementCaches_.topCounter.lastText));
    elementCaches_.topCounter.lastText[sizeof(elementCaches_.topCounter.lastText) - 1] = '\0';

    // Use bogey color for digits, muted color if muted, otherwise bogey color
    const bool allDigits = primaryIsDigit && secondaryIsDigit;
    uint16_t color;
    if (allDigits) {
        color = s.colorBogey;
    } else {
        color = muted ? PALETTE_MUTED_OR_PERSISTED : s.colorBogey;
    }

    // Fixed field clear every update prevents stale pixels from variable-width glyphs.
    // Clear rect spans from one column left of the field through the 6-px gap before
    // the band-label clear column (x=77). Without this, antialiased OFR halos or any
    // earlier render that touched x=72..76 (e.g. a previous fallback 7-segment draw,
    // boot-time residue) would persist forever — that gap is otherwise never cleared
    // by any element.
    FILL_RECT(clearX, TOP_COUNTER_FIELD_Y, clearW, TOP_COUNTER_FIELD_H, PALETTE_BG);

    const int centerBiasPx = 2;
    const int fieldLeft = TOP_COUNTER_FIELD_X + 1;
    const int fieldRight = TOP_COUNTER_FIELD_X + TOP_COUNTER_FIELD_W - TOP_COUNTER_PAD_RIGHT;
    const int clearRight = clearX + clearW;

    bool drewWithOfr = false;
    const int fontSize = hasSecondary ? 48 : TOP_COUNTER_FONT_SIZE;
    const int fallbackWidth = hasSecondary ? (TOP_COUNTER_FALLBACK_WIDTH * 2) : TOP_COUNTER_FALLBACK_WIDTH;
    const int textY = TOP_COUNTER_TEXT_Y + ((TOP_COUNTER_FONT_SIZE - fontSize) / 2);

    if (fontMgr_.segment7Ready) {
        // Use the Segment7 TTF for the intended visual style, but do NOT
        // center the cursor from the currently displayed glyph. Narrow glyphs
        // such as '1' and transient mode/volume symbols then appear to move
        // because their bounding boxes differ. Anchor the cursor to the fixed
        // physical LED cell using '8' as the canonical full-cell glyph.
        fontMgr_.segment7.setFontSize(fontSize);
        FT_BBox bbox = fontMgr_.segment7.calculateBoundingBox(
            0, 0, fontSize, Align::Left, Layout::Horizontal, buf);
        const int glyphXMin = static_cast<int>(bbox.xMin);
        const int glyphXMax = static_cast<int>(bbox.xMax);

        int x = TOP_COUNTER_FIELD_X + ((TOP_COUNTER_FIELD_W - fallbackWidth) / 2);
        if (!hasSecondary) {
            int refMin = 0;
            int refMax = 0;
            if (fontMgr_.getTopCounterBounds('8', false, refMin, refMax)) {
                const int fieldCenterX =
                    fieldLeft + ((fieldRight - fieldLeft) / 2) + centerBiasPx;
                const int refCenterX = refMin + ((refMax - refMin) / 2);
                x = fieldCenterX - refCenterX;
            }
        }

        const int minCursorX = fieldLeft - glyphXMin;
        const int maxCursorX = clearRight - glyphXMax;
        if (minCursorX <= maxCursorX) {
            if (x < minCursorX) x = minCursorX;
            if (x > maxCursorX) x = maxCursorX;
        } else {
            // Glyph is wider than the safe window; keep its left edge inside
            // rather than letting it drift into the band column.
            x = minCursorX;
        }

        // RGB565 → RGB888 via shared helper.
        const Rgb888 bg = rgb565ToRgb888(PALETTE_BG);
        const Rgb888 fg = rgb565ToRgb888(color);
        fontMgr_.segment7.setBackgroundColor(bg.r, bg.g, bg.b);
        fontMgr_.segment7.setFontColor(fg.r, fg.g, fg.b);
        fontMgr_.segment7.setCursor(x, textY);
        fontMgr_.segment7.printf("%s", buf);
        drewWithOfr = true;
    }

    if (!drewWithOfr) {
        const float scale = hasSecondary ? 1.8f : 2.2f;
        const int textWidth = measureSevenSegmentText(buf, scale);
        int x = TOP_COUNTER_FIELD_X + ((TOP_COUNTER_FIELD_W - textWidth) / 2) + centerBiasPx;
        if (x < fieldLeft) {
            x = fieldLeft;
        }
        if (x + textWidth > fieldRight && textWidth <= (fieldRight - fieldLeft)) {
            x = fieldRight - textWidth;
        }
        const int y = hasSecondary ? 16 : 10;
        drawSevenSegmentText(buf, x, y, scale, color, PALETTE_BG);
    }

    if (drawFixedPrimaryDot) {
        const int fieldRight = TOP_COUNTER_FIELD_X + TOP_COUNTER_FIELD_W - TOP_COUNTER_PAD_RIGHT;
        const int dotR = 4;
        // Pull the dot fully inside the field clear-rect so its right edge can never leak
        // pixels past the per-frame FILL_RECT (was: dotX = fieldRight - 2, which placed
        // dot pixels at columns just outside the cleared region and left a residual sliver
        // when the dot toggled off between frames).
        const int dotX = fieldRight - dotR - 1;
        const int dotY = TOP_COUNTER_TEXT_Y + TOP_COUNTER_FONT_SIZE - 5;
        FILL_CIRCLE(dotX, dotY, dotR, color);
    }
}

void V1Display::drawTopCounterSegment7(char symbol, bool muted, bool showDot) {
    drawTopCounterPair(symbol, muted, showDot);
}

// Router: bogey counter uses Segment7.
void V1Display::drawTopCounter(char symbol, bool muted, bool showDot) {
    drawTopCounterSegment7(symbol, muted, showDot);
}

// --- Mute icon badge ---

void V1Display::drawMuteIcon(bool muted) {
    // Skip redraw if nothing changed
    if (elementCaches_.topCounter.muteIconValid &&
        muted == elementCaches_.topCounter.lastMutedState) {
        return;
    }
    elementCaches_.topCounter.muteIconValid = true;
    elementCaches_.topCounter.lastMutedState = muted;

    // Draw badge at fixed top position (top ~10% of screen)
    const int leftMargin = 120;    // After band indicators
    const int rightMargin = 200;   // Before signal bars (at X=440)
    int maxWidth = SCREEN_WIDTH - leftMargin - rightMargin;

    int w = 110;
    int h = 26;
    int x = leftMargin + (maxWidth - w) / 2;  // Center between bands and signal bars
    int y = 5;  // Fixed near top of screen

    drawnRegion_.add(static_cast<int16_t>(x), static_cast<int16_t>(y),
                     static_cast<uint16_t>(w), static_cast<uint16_t>(h),
                         DisplayDirtyRegionSource::Status);

    if (muted) {
        // Draw badge with muted styling
        uint16_t outline = PALETTE_MUTED;
        uint16_t fill = PALETTE_MUTED;

        FILL_ROUND_RECT(x, y, w, h, 5, fill);
        DRAW_ROUND_RECT(x, y, w, h, 5, outline);

        GFX_setTextDatum(MC_DATUM);
        TFT_CALL(setTextSize)(2);  // Larger text for visibility
        TFT_CALL(setTextColor)(PALETTE_BG, fill);
        int cx = x + w / 2;
        int cy = y + h / 2;

        const char* muteText = "MUTED";
        // Pseudo-bold: draw twice with slight offset
        GFX_drawString(tft_, muteText, cx, cy);
        GFX_drawString(tft_, muteText, cx + 1, cy);
    } else {
        // Clear the badge area when not muted.
        FILL_RECT(leftMargin + (maxWidth - w) / 2, y, w, h, PALETTE_BG);
    }
}
