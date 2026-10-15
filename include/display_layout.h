// Waveshare 3.49-inch landscape geometry shared by renderers and visual assertions.

#pragma once

#ifndef SCREEN_WIDTH
#define SCREEN_WIDTH 640
#endif
#ifndef SCREEN_HEIGHT
#define SCREEN_HEIGHT 172
#endif

namespace DisplayLayout {

// Main display zone (shows frequency, status during alerts)
constexpr int PRIMARY_ZONE_HEIGHT = 95;
constexpr int PRIMARY_ZONE_Y = 20;      // Below status bar

// Secondary row (alert cards)
constexpr int SECONDARY_ROW_HEIGHT = 54;
constexpr int SECONDARY_ROW_Y = SCREEN_HEIGHT - SECONDARY_ROW_HEIGHT; // Y=118 on 172px display

// Primary drawing and clearing must stay above the card footer.
constexpr int CONTENT_BOTTOM_Y = SECONDARY_ROW_Y;

static_assert(PRIMARY_ZONE_Y + PRIMARY_ZONE_HEIGHT <= SECONDARY_ROW_Y, "Primary zone overlaps with secondary row");
static_assert(PRIMARY_ZONE_Y + PRIMARY_ZONE_HEIGHT <= CONTENT_BOTTOM_Y, "Primary zone exceeds content area");

constexpr int BAND_COLUMN_WIDTH = 120; // Width reserved for band labels (X, K, Ka, L)

constexpr int SIGNAL_COLUMN_WIDTH = 200; // Width reserved for signal bars, arrows, battery

constexpr int CONTENT_LEFT_MARGIN = BAND_COLUMN_WIDTH;    // 120px - after band indicators
constexpr int CONTENT_RIGHT_MARGIN = SIGNAL_COLUMN_WIDTH; // 200px - before signal column
constexpr int CONTENT_AVAILABLE_WIDTH = SCREEN_WIDTH - CONTENT_LEFT_MARGIN - CONTENT_RIGHT_MARGIN; // 320px

// SCAN shares live frequency geometry to avoid a jump on the first alert.
constexpr int FREQUENCY_MUTE_ICON_BOTTOM = 33;
constexpr int FREQUENCY_OFR_FONT_SIZE = 82;
constexpr int FREQUENCY_OFR_LEFT_MARGIN = 135;
constexpr int FREQUENCY_OFR_RIGHT_MARGIN = SIGNAL_COLUMN_WIDTH;
constexpr int FREQUENCY_OFR_Y_OFFSET = 13;
constexpr int FREQUENCY_FALLBACK_LEFT_MARGIN = CONTENT_LEFT_MARGIN;
constexpr int FREQUENCY_FALLBACK_RIGHT_MARGIN = SIGNAL_COLUMN_WIDTH;
constexpr float FREQUENCY_FALLBACK_SCALE = 2.5f;
constexpr int FREQUENCY_FALLBACK_Y_OFFSET = 11;

inline int frequencyOfrMaxWidth() {
    return SCREEN_WIDTH - FREQUENCY_OFR_LEFT_MARGIN - FREQUENCY_OFR_RIGHT_MARGIN;
}

inline int frequencyOfrY() {
    return FREQUENCY_MUTE_ICON_BOTTOM + (CONTENT_BOTTOM_Y - FREQUENCY_MUTE_ICON_BOTTOM - FREQUENCY_OFR_FONT_SIZE) / 2 +
           FREQUENCY_OFR_Y_OFFSET;
}

inline int frequencyFallbackMaxWidth() {
    return SCREEN_WIDTH - FREQUENCY_FALLBACK_LEFT_MARGIN - FREQUENCY_FALLBACK_RIGHT_MARGIN;
}

inline int frequencyFallbackY(int glyphHeight) {
    return FREQUENCY_MUTE_ICON_BOTTOM + (CONTENT_BOTTOM_Y - FREQUENCY_MUTE_ICON_BOTTOM - glyphHeight) / 2 +
           FREQUENCY_FALLBACK_Y_OFFSET;
}

constexpr int TOP_COUNTER_FONT_SIZE = 60; // Matches DisplayFontManager constant
constexpr int TOP_COUNTER_FIELD_X = 16;
constexpr int TOP_COUNTER_FIELD_Y = 6;
// One physical LED cell; image2 is its blink-off mask, not a second digit.
constexpr int TOP_COUNTER_FIELD_W = 55;
constexpr int TOP_COUNTER_FIELD_H = TOP_COUNTER_FONT_SIZE + 8;
constexpr int TOP_COUNTER_TEXT_Y = 8;
constexpr int TOP_COUNTER_PAD_RIGHT = 2;
constexpr int TOP_COUNTER_FALLBACK_WIDTH = 28;

struct DisplayRect {
    int16_t x;
    int16_t y;
    int16_t w;
    int16_t h;
};

// Coarse zone bounds for DrawnRegion, not clear/draw rectangles. Renderers may
// report tighter painted bounds instead.

constexpr DisplayRect kFrequencyZoneRect{
    static_cast<int16_t>(CONTENT_LEFT_MARGIN), static_cast<int16_t>(PRIMARY_ZONE_Y),
    static_cast<int16_t>(CONTENT_AVAILABLE_WIDTH), static_cast<int16_t>(PRIMARY_ZONE_HEIGHT)};

constexpr DisplayRect kBandsColumnRect{0, static_cast<int16_t>(PRIMARY_ZONE_Y), static_cast<int16_t>(BAND_COLUMN_WIDTH),
                                       static_cast<int16_t>(PRIMARY_ZONE_HEIGHT)};

// Covers the whole signal column; the six-bar stack extends below the primary zone.
constexpr DisplayRect kSignalBarsRect{static_cast<int16_t>(SCREEN_WIDTH - SIGNAL_COLUMN_WIDTH), 18,
                                      static_cast<int16_t>(SIGNAL_COLUMN_WIDTH), 134};

// Covers both card slots regardless of occupancy.
constexpr DisplayRect kSecondaryCardsRect{0, static_cast<int16_t>(SECONDARY_ROW_Y), static_cast<int16_t>(SCREEN_WIDTH),
                                          static_cast<int16_t>(SECONDARY_ROW_HEIGHT)};

constexpr DisplayRect kTopCounterRect{
    static_cast<int16_t>(TOP_COUNTER_FIELD_X), static_cast<int16_t>(TOP_COUNTER_FIELD_Y),
    static_cast<int16_t>(TOP_COUNTER_FIELD_W), static_cast<int16_t>(TOP_COUNTER_FIELD_H)};

// The visual manifest exports these same helpers. AssertRect helpers describe
// verification probes; other rectangles describe drawing and clearing bounds.

constexpr int BAND_CELL_COUNT = 4;

inline DisplayRect bandCellAssertRect(int index) {
    // FreeSansBold24pt7b glyph probes. These intentionally exclude the broader
    // per-cell clear windows because L overlaps the GPS badge and X overlaps
    // card slot 0. Keep these font-derived boxes paired with the renderer's
    // fixed x=82 and y=55+43*i anchors. Ku is a display-only K-cell label
    // shifted left of this K probe so its extra letter stays clear of card 0.
    const int clamped = (index < 0) ? 0 : ((index >= BAND_CELL_COUNT) ? (BAND_CELL_COUNT - 1) : index);
    switch (clamped) {
    case 0:
        return DisplayRect{82, 5, 23, 34};
    case 1:
        return DisplayRect{82, 48, 56, 35};
    case 2:
        return DisplayRect{82, 91, 30, 35};
    default:
        return DisplayRect{82, 134, 30, 34};
    }
}

constexpr int MAIN_SIGNAL_BAR_COUNT = 6;

inline DisplayRect mainSignalBarRect(int index) {
    constexpr int kBarWidth = 44;
    constexpr int kBarHeight = 14;
    constexpr int kBarSpacing = 10;
    constexpr int kStartX = SCREEN_WIDTH - SIGNAL_COLUMN_WIDTH;
    constexpr int kStartY = 18;
    const int clamped = (index < 0) ? 0 : ((index >= MAIN_SIGNAL_BAR_COUNT) ? (MAIN_SIGNAL_BAR_COUNT - 1) : index);
    const int visualIndex = MAIN_SIGNAL_BAR_COUNT - 1 - clamped;
    return DisplayRect{
        static_cast<int16_t>(kStartX),
        static_cast<int16_t>(kStartY + visualIndex * (kBarHeight + kBarSpacing)),
        static_cast<int16_t>(kBarWidth),
        static_cast<int16_t>(kBarHeight),
    };
}

inline DisplayRect mainSignalBarsRect() {
    const DisplayRect top = mainSignalBarRect(MAIN_SIGNAL_BAR_COUNT - 1);
    const DisplayRect bottom = mainSignalBarRect(0);
    return DisplayRect{
        top.x,
        top.y,
        top.w,
        static_cast<int16_t>(bottom.y + bottom.h - top.y),
    };
}

constexpr int CARD_SLOT_COUNT = 2;
// Six physical segments, matching V1G2, Concealed Display, and Tech Display.
// The alert parser keeps VR's 0-8 diagnostic strength; the visual contract
// projects that range onto these six cells.
constexpr int CARD_METER_BAR_COUNT = 6;
constexpr int CARD_TEXT_PROBE_TOP_PAD = 2;

inline DisplayRect cardRect(int slot) {
    constexpr int kCardW = 145;
    constexpr int kCardSpacing = 10;
    constexpr int kLeftMargin = CONTENT_LEFT_MARGIN;
    constexpr int kSignalBarsX = SCREEN_WIDTH - SIGNAL_COLUMN_WIDTH;
    constexpr int kAvailableWidth = kSignalBarsX - kLeftMargin;
    constexpr int kTotalCardsWidth = kCardW * CARD_SLOT_COUNT + kCardSpacing;
    constexpr int kStartX = kLeftMargin + (kAvailableWidth - kTotalCardsWidth) / 2;
    const int clamped = (slot < 0) ? 0 : ((slot >= CARD_SLOT_COUNT) ? (CARD_SLOT_COUNT - 1) : slot);
    return DisplayRect{
        static_cast<int16_t>(kStartX + clamped * (kCardW + kCardSpacing)),
        static_cast<int16_t>(SECONDARY_ROW_Y),
        static_cast<int16_t>(kCardW),
        static_cast<int16_t>(SECONDARY_ROW_HEIGHT),
    };
}

inline DisplayRect cardsClearRect() {
    // Clear 8 px past slot 1, stopping 2 px before the signal-bar lane.
    const DisplayRect first = cardRect(0);
    constexpr int kClearRight = SCREEN_WIDTH - SIGNAL_COLUMN_WIDTH - 2;
    return DisplayRect{
        first.x,
        first.y,
        static_cast<int16_t>(kClearRight - first.x),
        first.h,
    };
}

inline DisplayRect cardTextRect(int slot) {
    // Built-in font assertion window. It begins two rows above the renderer's
    // cursor to include the complete glyph raster.
    const DisplayRect card = cardRect(slot);
    return DisplayRect{
        static_cast<int16_t>(card.x + 36),
        static_cast<int16_t>(card.y + 9),
        static_cast<int16_t>(card.w - 44),
        22,
    };
}

inline int cardTextCursorY(int slot) {
    return cardTextRect(slot).y + CARD_TEXT_PROBE_TOP_PAD;
}

inline DisplayRect cardMeterRect(int slot) {
    const DisplayRect card = cardRect(slot);
    return DisplayRect{
        static_cast<int16_t>(card.x + 10),
        static_cast<int16_t>(card.y + 34),
        static_cast<int16_t>(card.w - 20),
        18,
    };
}

inline DisplayRect cardMeterBarRect(int slot, int index) {
    constexpr int kBarSpacing = 2;
    constexpr int kBarH = 10;
    const DisplayRect meter = cardMeterRect(slot);
    const int barW = (meter.w - (CARD_METER_BAR_COUNT - 1) * kBarSpacing) / CARD_METER_BAR_COUNT;
    const int clamped = (index < 0) ? 0 : ((index >= CARD_METER_BAR_COUNT) ? (CARD_METER_BAR_COUNT - 1) : index);
    return DisplayRect{
        static_cast<int16_t>(meter.x + clamped * (barW + kBarSpacing)),
        static_cast<int16_t>(meter.y + (meter.h - kBarH) / 2),
        static_cast<int16_t>(barW),
        static_cast<int16_t>(kBarH),
    };
}

inline DisplayRect cardMeterAssertRect(int slot, int index) {
    // Inactive bars are outlines, so the shared top edge is the only probe
    // covered by the expected role for both active and inactive states.
    DisplayRect rect = cardMeterBarRect(slot, index);
    rect.h = 1;
    return rect;
}

inline DisplayRect frequencyTextRect() {
    return DisplayRect{
        static_cast<int16_t>(FREQUENCY_OFR_LEFT_MARGIN),
        static_cast<int16_t>(FREQUENCY_MUTE_ICON_BOTTOM),
        static_cast<int16_t>(frequencyOfrMaxWidth()),
        static_cast<int16_t>(CONTENT_BOTTOM_Y - FREQUENCY_MUTE_ICON_BOTTOM),
    };
}

inline DisplayRect directionArrowClusterRect(bool raisedLayout) {
    const int cxBase = SCREEN_WIDTH - 70 - 6;
    const int cy = raisedLayout ? 94 : 104;
    constexpr float scale = 0.98f;
    const int topW = static_cast<int>(125 * scale);
    const int topH = static_cast<int>(70 * scale);
    const int bottomW = static_cast<int>(125 * scale);
    const int bottomH = static_cast<int>(30 * scale);
    const int sideBarH = static_cast<int>(22 * scale);
    const int gap = static_cast<int>(15 * scale);
    const int topArrowCenterY = cy - sideBarH / 2 - gap - topH / 2;
    const int bottomArrowCenterY = cy + sideBarH / 2 + gap + bottomH / 2;
    const int maxW = (topW > bottomW) ? topW : bottomW;
    int clearLeft = cxBase - maxW / 2 - 10;
    int clearWidth = maxW + 20;
    if (!raisedLayout) {
        const int maxClearRight = SCREEN_WIDTH - 42;
        if (clearLeft + clearWidth > maxClearRight) {
            clearWidth = maxClearRight - clearLeft;
        }
    }
    if (clearLeft + clearWidth > SCREEN_WIDTH) {
        clearWidth = SCREEN_WIDTH - clearLeft;
    }
    int totalTop = topArrowCenterY - topH / 2 - 2;
    int totalBottom = bottomArrowCenterY + bottomH / 2 + 2;
    if (totalTop < 0)
        totalTop = 0;
    if (totalBottom > SCREEN_HEIGHT)
        totalBottom = SCREEN_HEIGHT;
    return DisplayRect{
        static_cast<int16_t>(clearLeft),
        static_cast<int16_t>(totalTop),
        static_cast<int16_t>(clearWidth),
        static_cast<int16_t>(totalBottom - totalTop),
    };
}

inline DisplayRect directionArrowAssertRect(int directionMask, bool raisedLayout) {
    const int cx = SCREEN_WIDTH - 70 - 6;
    const int cy = raisedLayout ? 94 : 104;
    constexpr float scale = 0.98f;
    const int topH = static_cast<int>(70 * scale);
    const int sideBarH = static_cast<int>(22 * scale);
    const int gap = static_cast<int>(15 * scale);
    const int topArrowCenterY = cy - sideBarH / 2 - gap - topH / 2;
    const int bottomH = static_cast<int>(30 * scale);
    const int bottomArrowCenterY = cy + sideBarH / 2 + gap + bottomH / 2;
    if (directionMask == 0x01) {
        return DisplayRect{static_cast<int16_t>(cx - 10), static_cast<int16_t>(topArrowCenterY + 16), 20, 10};
    }
    if (directionMask == 0x02) {
        return DisplayRect{static_cast<int16_t>(cx - 28), static_cast<int16_t>(cy - 6), 56, 12};
    }
    return DisplayRect{static_cast<int16_t>(cx - 10), static_cast<int16_t>(bottomArrowCenterY - 9), 20, 8};
}

inline DisplayRect volumeMainRect() {
    return DisplayRect{8, 75, 34, 16};
}

inline DisplayRect volumeMuteRect() {
    return DisplayRect{44, 75, 39, 16};
}

inline DisplayRect volumeIndicatorRect() {
    const DisplayRect main = volumeMainRect();
    const DisplayRect mute = volumeMuteRect();
    return DisplayRect{
        main.x,
        main.y,
        static_cast<int16_t>(mute.x + mute.w - main.x),
        main.h,
    };
}

inline DisplayRect profileRect() {
    constexpr int kCx = SCREEN_WIDTH - 70 - 6;
    constexpr int kClearW = 130;
    return DisplayRect{static_cast<int16_t>(kCx - kClearW / 2), 152, kClearW, 20};
}

inline DisplayRect profileMaxTextRect() {
    // Profile names permit 20 built-in-font characters centered at x=564;
    // clipping to the framebuffer yields the worst-case on-screen text span.
    return DisplayRect{444, 152, 196, 20};
}

inline DisplayRect alpBadgeRect() {
    return DisplayRect{170, 5, 50, 26};
}

inline DisplayRect obdBadgeRect() {
    return DisplayRect{370, 5, 50, 26};
}

inline DisplayRect gpsBadgeRect() {
    return DisplayRect{120, 5, 46, 26};
}

inline DisplayRect rssiRect() {
    return DisplayRect{8, 99, 70, 44};
}

inline DisplayRect bleBadgeRect() {
    return DisplayRect{38, 143, 24, 24};
}

inline DisplayRect wifiBadgeRect() {
    return DisplayRect{6, 143, 24, 24};
}

constexpr int STATUS_ICON_CLEAR_PAD = 2;

inline DisplayRect bleIconRect() {
    const DisplayRect badge = bleBadgeRect();
    return DisplayRect{
        static_cast<int16_t>(badge.x + STATUS_ICON_CLEAR_PAD),
        static_cast<int16_t>(badge.y + STATUS_ICON_CLEAR_PAD),
        static_cast<int16_t>(badge.w - 2 * STATUS_ICON_CLEAR_PAD),
        static_cast<int16_t>(badge.h - 2 * STATUS_ICON_CLEAR_PAD),
    };
}

inline DisplayRect wifiIconRect() {
    const DisplayRect badge = wifiBadgeRect();
    return DisplayRect{
        static_cast<int16_t>(badge.x + STATUS_ICON_CLEAR_PAD),
        static_cast<int16_t>(badge.y + STATUS_ICON_CLEAR_PAD),
        static_cast<int16_t>(badge.w - 2 * STATUS_ICON_CLEAR_PAD),
        static_cast<int16_t>(badge.h - 2 * STATUS_ICON_CLEAR_PAD),
    };
}

inline DisplayRect muteBadgeRect() {
    return DisplayRect{225, 5, 110, 26};
}

inline DisplayRect muteBadgeAssertRect() {
    // Interior fill probe used by the verifier; excludes rounded corners and
    // most of the overlaid MUTED text.
    return DisplayRect{232, 8, 12, 20};
}

inline DisplayRect batteryPercentRect() {
    return DisplayRect{590, 0, 48, 30};
}

inline DisplayRect batteryIconRect() {
    return DisplayRect{616, 129, 18, 37};
}

} // namespace DisplayLayout

inline int getEffectiveScreenHeight() {
    return DisplayLayout::PRIMARY_ZONE_HEIGHT;
}
