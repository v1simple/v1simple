#include "display.h"
#include "display_layout.h"
#include "display_draw.h"
#include "display_element_caches.h"
#include "display_palette.h"
#include "display_text.h"
#include "display_segments.h"
#include "display_font_manager.h"
#include "settings.h"
#include <cstring>

using namespace DisplaySegments;

using DisplayLayout::TOP_COUNTER_FALLBACK_WIDTH;
using DisplayLayout::TOP_COUNTER_FIELD_H;
using DisplayLayout::TOP_COUNTER_FIELD_W;
using DisplayLayout::TOP_COUNTER_FIELD_X;
using DisplayLayout::TOP_COUNTER_FIELD_Y;
using DisplayLayout::TOP_COUNTER_FONT_SIZE;
using DisplayLayout::TOP_COUNTER_PAD_RIGHT;
using DisplayLayout::TOP_COUNTER_TEXT_Y;

namespace {

bool hasTrailingTopCounterSymbol(char symbol, bool showDot) {
    return (symbol != '\0' && symbol != ' ') || showDot;
}

bool isTopCounterDigit(char symbol) {
    return symbol >= '0' && symbol <= '9';
}

void buildTopCounterText(char primary, bool primaryDot, char secondary, bool secondaryDot, bool drawFixedPrimaryDot,
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

} // namespace

// --- 7-segment digit rendering ---

void V1Display::drawSevenSegmentDigit(int x, int y, float scale, char c, bool addDot, uint16_t onColor,
                                      uint16_t offColor) {
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
        segments[0] = segments[1] = segments[2] = segments[4] = segments[5] = segments[6] = true;
    } else if (c == 'L') {
        segments[3] = segments[4] = segments[5] = true;
    } else if (c == 'l' || c == '&') {
        // Logic (lowercase) L: bottom + lower-left only
        // '&' is used in decoder for little L (logic mode, byte value 24)
        segments[3] = segments[4] = true;
    } else if (c == 'S' || c == 's') {
        segments[0] = segments[5] = segments[6] = segments[2] = segments[3] = true;
    } else if (c == 'E') {
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
        segments[0] = segments[5] = segments[6] = segments[4] = true;
    } else if (c == 'C') {
        segments[0] = segments[5] = segments[4] = segments[3] = true;
    } else if (c == 'U') {
        segments[1] = segments[2] = segments[3] = segments[4] = segments[5] = true;
    } else if (c == 'u') {
        segments[2] = segments[3] = segments[4] = true;
    } else if (c == 'b') {
        segments[5] = segments[4] = segments[3] = segments[2] = segments[6] = true;
    } else if (c == 'c') {
        segments[6] = segments[4] = segments[3] = true;
    } else if (c == 'd') {
        segments[1] = segments[2] = segments[3] = segments[4] = segments[6] = true;
    } else if (c == 'e') {
        segments[0] = segments[5] = segments[4] = segments[3] = segments[6] = true;
    }

    auto drawSeg = [&](int sx, int sy, int w, int h, bool on) {
        uint16_t col = on ? onColor : offColor;
        if (!on && offColor == PALETTE_BG)
            return;
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
        if (text[i] == '.')
            continue;
        bool hasDot = (i + 1 < len && text[i + 1] == '.');
        width += m.digitW + m.spacing + (hasDot ? m.dot / 2 : 0);
        if (hasDot)
            ++i;
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
        if (c == '.')
            continue; // handled alongside previous digit
        bool hasDot = (i + 1 < len && text[i + 1] == '.');
        drawSevenSegmentDigit(cursor, y, scale, c, hasDot, onColor, offColor);
        cursor += m.digitW + m.spacing + (hasDot ? m.dot / 2 : 0);
        if (hasDot)
            ++i;
    }
    return cursor - x - m.spacing;
}

// --- 14-segment digit and text rendering ---

void V1Display::draw14SegmentDigit(int x, int y, float scale, char c, bool addDot, uint16_t onColor,
                                   uint16_t offColor) {
    SegMetrics m = segMetrics(scale);
    uint16_t pattern = get14SegPattern(c);

    auto drawHSeg = [&](int sx, int sy, int w, bool on) {
        uint16_t col = on ? onColor : offColor;
        if (!on && offColor == PALETTE_BG)
            return;
        FILL_ROUND_RECT(sx, sy, w, m.segThick, scale, col);
    };

    auto drawVSeg = [&](int sx, int sy, int h, bool on) {
        uint16_t col = on ? onColor : offColor;
        if (!on && offColor == PALETTE_BG)
            return;
        FILL_ROUND_RECT(sx, sy, m.segThick, h, scale, col);
    };

    auto drawDiag = [&](int x1, int y1, int x2, int y2, bool on) {
        uint16_t col = on ? onColor : offColor;
        if (!on && offColor == PALETTE_BG)
            return;
        for (int t = -m.segThick / 2; t <= m.segThick / 2; t++) {
            DRAW_LINE(x1 + t, y1, x2 + t, y2, col);
            DRAW_LINE(x1, y1 + t, x2, y2 + t, col);
        }
    };

    int halfW = m.segLen / 2;
    int centerX = x + m.segThick + halfW;
    int midY = y + m.segLen + m.segThick;

    // Horizontal segments
    drawHSeg(x + m.segThick, y, m.segLen, pattern & S14_TOP);                                 // Top
    drawHSeg(x + m.segThick, y + 2 * m.segLen + 2 * m.segThick, m.segLen, pattern & S14_BOT); // Bottom
    drawHSeg(x + m.segThick, midY, halfW - m.segThick / 2, pattern & S14_ML);                 // Middle-left
    drawHSeg(centerX + m.segThick / 2, midY, halfW - m.segThick / 2, pattern & S14_MR);       // Middle-right

    // Vertical segments - outer
    drawVSeg(x, y + m.segThick, m.segLen, pattern & S14_TL);                                        // Top-left
    drawVSeg(x, y + m.segLen + 2 * m.segThick, m.segLen, pattern & S14_BL);                         // Bottom-left
    drawVSeg(x + m.segLen + m.segThick, y + m.segThick, m.segLen, pattern & S14_TR);                // Top-right
    drawVSeg(x + m.segLen + m.segThick, y + m.segLen + 2 * m.segThick, m.segLen, pattern & S14_BR); // Bottom-right

    // Center vertical segments
    drawVSeg(centerX, y + m.segThick, m.segLen - m.segThick, pattern & S14_CT);    // Center-top
    drawVSeg(centerX, midY + m.segThick, m.segLen - m.segThick, pattern & S14_CB); // Center-bottom

    // Diagonal segments
    int diagInset = m.segThick;
    drawDiag(x + diagInset, y + m.segThick + diagInset, centerX - diagInset, midY - diagInset,
             pattern & S14_DTL); // Diag top-left
    drawDiag(centerX + diagInset, y + m.segThick + diagInset, x + m.segLen + m.segThick - diagInset, midY - diagInset,
             pattern & S14_DTR); // Diag top-right
    drawDiag(x + diagInset, y + 2 * m.segLen + m.segThick - diagInset, centerX - diagInset,
             midY + m.segThick + diagInset, pattern & S14_DBL); // Diag bottom-left
    drawDiag(centerX + diagInset, midY + m.segThick + diagInset, x + m.segLen + m.segThick - diagInset,
             y + 2 * m.segLen + m.segThick - diagInset, pattern & S14_DBR); // Diag bottom-right

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
        if (c == '.')
            continue;
        bool hasDot = (i + 1 < len && text[i + 1] == '.');
        draw14SegmentDigit(cursor, y, scale, c, hasDot, onColor, offColor);
        cursor += m.digitW + m.spacing + (hasDot ? m.dot / 2 : 0);
        if (hasDot)
            ++i;
    }
    return cursor - x - m.spacing;
}

void V1Display::drawTopCounterPair(char primary, bool muted, bool primaryDot, char secondary, bool secondaryDot) {
    const V1Settings& s = settings_.get();
    const bool hasSecondary = hasTrailingTopCounterSymbol(secondary, secondaryDot);
    const bool primaryIsDigit = isTopCounterDigit(primary);
    const bool secondaryIsDigit = !hasSecondary || isTopCounterDigit(secondary);
    const bool drawFixedPrimaryDot = !hasSecondary && primaryIsDigit && primaryDot;
    char buf[8] = {0};
    buildTopCounterText(primary, primaryDot, secondary, secondaryDot, drawFixedPrimaryDot, buf);
    const bool hasPhotoIdentity = strchr(buf, 'P') != nullptr;
    const uint16_t configuredColor = hasPhotoIdentity ? s.colorBandPhoto : s.colorBogey;

    bool colorChanged = (configuredColor != elementCaches_.topCounter.lastBogeyColor);

    if (elementCaches_.topCounter.counterValid && !colorChanged && muted == elementCaches_.topCounter.lastMuted &&
        drawFixedPrimaryDot == elementCaches_.topCounter.lastFixedPrimaryDot &&
        strcmp(buf, elementCaches_.topCounter.lastText) == 0) {
        return;
    }
    // Include the gap before the band-label clear column; no other renderer
    // clears counter residue there.
    constexpr int kBandsColumnLeftX = 77; // mirrors unionX in drawBandIndicators (x=82, x-5=77)
    constexpr int kClearLeftPad = 1;
    const int clearX = TOP_COUNTER_FIELD_X - kClearLeftPad;
    const int clearW = (kBandsColumnLeftX - clearX);
    drawnRegion_.add(static_cast<int16_t>(clearX), DisplayLayout::kTopCounterRect.y, static_cast<int16_t>(clearW),
                     DisplayLayout::kTopCounterRect.h);
    elementCaches_.topCounter.counterValid = true;
    elementCaches_.topCounter.lastMuted = muted;
    elementCaches_.topCounter.lastFixedPrimaryDot = drawFixedPrimaryDot;
    elementCaches_.topCounter.lastBogeyColor = configuredColor;
    strncpy(elementCaches_.topCounter.lastText, buf, sizeof(elementCaches_.topCounter.lastText));
    elementCaches_.topCounter.lastText[sizeof(elementCaches_.topCounter.lastText) - 1] = '\0';

    // P keeps the dedicated Photo color; numeric counts retain the bogey color.
    // Muting takes precedence for symbols.
    const bool allDigits = primaryIsDigit && secondaryIsDigit;
    uint16_t color;
    if (allDigits) {
        color = s.colorBogey;
    } else {
        color = muted ? PALETTE_MUTED_OR_PERSISTED : configuredColor;
    }

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
        // Anchor the cursor to '8', not the current glyph's variable width.
        // Use boot-primed bounds: live OFR measurements have shifted under cache
        // pressure after FreeType errors. Only the rare two-symbol size-48 layout
        // falls back to live measurement because the bounds cache does not cover it.
        fontMgr_.segment7.setFontSize(fontSize);
        int glyphXMin = 0;
        int glyphXMax = 0;
        bool haveCachedBounds = false;
        if (!hasSecondary) {
            haveCachedBounds = fontMgr_.getTopCounterBounds(buf[0], buf[1] == '.', glyphXMin, glyphXMax);
        }
        if (!haveCachedBounds) {
            FT_BBox bbox = fontMgr_.segment7.calculateBoundingBox(0, 0, fontSize, Align::Left, Layout::Horizontal, buf);
            glyphXMin = static_cast<int>(bbox.xMin);
            glyphXMax = static_cast<int>(bbox.xMax);
        }

        int x = TOP_COUNTER_FIELD_X + ((TOP_COUNTER_FIELD_W - fallbackWidth) / 2);
        if (!hasSecondary) {
            int refMin = 0;
            int refMax = 0;
            if (fontMgr_.getTopCounterBounds('8', false, refMin, refMax)) {
                const int fieldCenterX = fieldLeft + ((fieldRight - fieldLeft) / 2) + centerBiasPx;
                const int refCenterX = refMin + ((refMax - refMin) / 2);
                x = fieldCenterX - refCenterX;
            }
        }

        const int minCursorX = fieldLeft - glyphXMin;
        const int maxCursorX = clearRight - glyphXMax;
        if (minCursorX <= maxCursorX) {
            if (x < minCursorX)
                x = minCursorX;
            if (x > maxCursorX)
                x = maxCursorX;
        } else {
            // Glyph is wider than the safe window; keep its left edge inside
            // rather than letting it drift into the band column.
            x = minCursorX;
        }

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
        // Keep the dot inside the clear rect so toggling it off leaves no residue.
        const int dotX = fieldRight - dotR - 1;
        const int dotY = TOP_COUNTER_TEXT_Y + TOP_COUNTER_FONT_SIZE - 5;
        FILL_CIRCLE(dotX, dotY, dotR, color);
    }
}

void V1Display::drawTopCounterSegment7(char symbol, bool muted, bool showDot) {
    drawTopCounterPair(symbol, muted, showDot);
}

void V1Display::drawTopCounter(char symbol, bool muted, bool showDot) {
    drawTopCounterSegment7(symbol, muted, showDot);
}

void V1Display::drawMuteIcon(bool muted) {
    if (elementCaches_.topCounter.muteIconValid && muted == elementCaches_.topCounter.lastMutedState) {
        return;
    }
    elementCaches_.topCounter.muteIconValid = true;
    elementCaches_.topCounter.lastMutedState = muted;

    const DisplayLayout::DisplayRect badgeRect = DisplayLayout::muteBadgeRect();

    drawnRegion_.add(badgeRect.x, badgeRect.y, badgeRect.w, badgeRect.h);

    if (muted) {
        uint16_t outline = PALETTE_MUTED;
        uint16_t fill = PALETTE_MUTED;

        FILL_ROUND_RECT(badgeRect.x, badgeRect.y, badgeRect.w, badgeRect.h, 5, fill);
        DRAW_ROUND_RECT(badgeRect.x, badgeRect.y, badgeRect.w, badgeRect.h, 5, outline);

        GFX_setTextDatum(MC_DATUM);
        TFT_CALL(setTextSize)(2); // Larger text for visibility
        TFT_CALL(setTextColor)(PALETTE_BG, fill);
        int cx = badgeRect.x + badgeRect.w / 2;
        int cy = badgeRect.y + badgeRect.h / 2;

        const char* muteText = "MUTED";
        // Pseudo-bold: draw twice with slight offset
        GFX_drawString(tft_, muteText, cx, cy);
        GFX_drawString(tft_, muteText, cx + 1, cy);
    } else {
        FILL_RECT(badgeRect.x, badgeRect.y, badgeRect.w, badgeRect.h, PALETTE_BG);
    }
}
