/**
 * Display Layout Constants for V1 Gen2 Display
 * Waveshare ESP32-S3-Touch-LCD-3.49 (640x172 LCD)
 *
 * Centralizes all layout-related constants to ensure consistency
 * across display.cpp and tests. Derived from config.h screen dimensions.
 *
 * Layout Overview (640x172 landscape) — approximate lanes; exact rects live
 * in each leaf's draw code (grep the FILL_RECT/drawnRegion_ calls):
 * ┌────────────────────────────────────────────────────────────────────────────┐
 * │ [GPS x120][ALP x170][MUTED x225][OBD x370..420]                [Batt x590+]│ Y=0-31
 * ├──────────┬──────────────────────────────────┬──────────┬───────────────────┤
 * │ Vol/RSSI │                                  │ Signal   │  Direction        │
 * │ (x=8)    │  Frequency / Alert Content       │ Bars     │  Arrows           │ mid
 * │ Band     │  (x=120..440)                    │ x440-484 │  x493-635         │
 * │ Labels   │                                  │ y=10-165 │  y=0-158          │
 * │ (x77-143)│                                  │          │                   │
 * ├──────────┴──────────────────────────────────┴──────────┴───────────────────┤
 * │ [WiFi x8] [Card 1][Card 2] (145x54, x130-430)      [Profile x499-629 y152+]│ Y=118-172
 * └────────────────────────────────────────────────────────────────────────────┘
 */

#pragma once

// ============================================================================
// Screen Dimensions (from platformio.ini build flags)
// ============================================================================
// SCREEN_WIDTH and SCREEN_HEIGHT are defined via -D flags in platformio.ini
// Default values if not defined:
#ifndef SCREEN_WIDTH
#define SCREEN_WIDTH 640
#endif
#ifndef SCREEN_HEIGHT
#define SCREEN_HEIGHT 172
#endif

// ============================================================================
// Primary Layout Zones
// ============================================================================

namespace DisplayLayout {

// Main display zone (shows frequency, status during alerts)
constexpr int PRIMARY_ZONE_HEIGHT = 95;   // Fixed height for primary alert display
constexpr int PRIMARY_ZONE_Y = 20;        // Below status bar

// Secondary row (alert cards)
constexpr int SECONDARY_ROW_HEIGHT = 54;  // Height for secondary alert cards
constexpr int SECONDARY_ROW_Y = SCREEN_HEIGHT - SECONDARY_ROW_HEIGHT;  // Y=118 on 172px display

// Bottom boundary for primary content — the secondary cards row owns the
// footer, so primary content (frequency numerals, alert text, clears) must
// stay above SECONDARY_ROW_Y.
constexpr int CONTENT_BOTTOM_Y = SECONDARY_ROW_Y;

// Verify zones don't overlap (compile-time check)
static_assert(PRIMARY_ZONE_Y + PRIMARY_ZONE_HEIGHT <= SECONDARY_ROW_Y,
              "Primary zone overlaps with secondary row");
static_assert(PRIMARY_ZONE_Y + PRIMARY_ZONE_HEIGHT <= CONTENT_BOTTOM_Y,
              "Primary zone exceeds content area");

// ============================================================================
// Band Indicator Column (Left Side)
// ============================================================================

constexpr int BAND_COLUMN_WIDTH = 120;    // Width reserved for band labels (X, K, Ka, L)

// ============================================================================
// Signal/Info Column (Right Side)
// ============================================================================

constexpr int SIGNAL_COLUMN_WIDTH = 200;  // Width reserved for signal bars, arrows, battery

// ============================================================================
// Frequency/Content Area (Center)
// ============================================================================

// Content area margins define the frequency display region
constexpr int CONTENT_LEFT_MARGIN = BAND_COLUMN_WIDTH;     // 120px - after band indicators
constexpr int CONTENT_RIGHT_MARGIN = SIGNAL_COLUMN_WIDTH;  // 200px - before signal column
constexpr int CONTENT_AVAILABLE_WIDTH = SCREEN_WIDTH - CONTENT_LEFT_MARGIN - CONTENT_RIGHT_MARGIN;  // 320px

// Shared frequency/SCAN geometry.  SCAN intentionally uses the same baseline,
// font size, and horizontal lane as live frequency text so the disconnected
// search screen does not visually jump when the first alert arrives.
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
    return FREQUENCY_MUTE_ICON_BOTTOM +
           (CONTENT_BOTTOM_Y - FREQUENCY_MUTE_ICON_BOTTOM - FREQUENCY_OFR_FONT_SIZE) / 2 +
           FREQUENCY_OFR_Y_OFFSET;
}

inline int frequencyFallbackMaxWidth() {
    return SCREEN_WIDTH - FREQUENCY_FALLBACK_LEFT_MARGIN - FREQUENCY_FALLBACK_RIGHT_MARGIN;
}

inline int frequencyFallbackY(int glyphHeight) {
    return FREQUENCY_MUTE_ICON_BOTTOM +
           (CONTENT_BOTTOM_Y - FREQUENCY_MUTE_ICON_BOTTOM - glyphHeight) / 2 +
           FREQUENCY_FALLBACK_Y_OFFSET;
}

// ============================================================================
// Top Counter Area
// ============================================================================

constexpr int TOP_COUNTER_FONT_SIZE = 60; // Matches DisplayFontManager constant
constexpr int TOP_COUNTER_FIELD_X = 16;
constexpr int TOP_COUNTER_FIELD_Y = 6;
// Single-digit field. Sized so the clear rect (X=16..71) leaves a 6px gap
// before the band-label clear rect at X=77 (see drawBandIndicators in
// src/display_bands.cpp: x=82, unionX=x-5=77, labelClearW=50). Rendering
// uses a single LED with image2 as the blink-off mask, so the field is
// sized for a single-digit width.
constexpr int TOP_COUNTER_FIELD_W = 55;
constexpr int TOP_COUNTER_FIELD_H = TOP_COUNTER_FONT_SIZE + 8;
constexpr int TOP_COUNTER_TEXT_Y = 8;
constexpr int TOP_COUNTER_PAD_RIGHT = 2;
constexpr int TOP_COUNTER_FALLBACK_WIDTH = 28;

// ============================================================================
// DisplayRect — shared rect struct for partial-flush bookkeeping
// ============================================================================
// Generic rect struct used by partial-flush helpers (arrowBoundingRect, leaf
// draw functions contributing to DrawnRegion). Formerly V1Display::ArrowClusterRect;
// promoted to the layout header so every leaf can produce one without
// depending on display.h.

struct DisplayRect {
    int16_t x;
    int16_t y;
    int16_t w;
    int16_t h;
};

// ============================================================================
// Zone rects — coarse bounding rectangles for partial-flush dispatch
// ============================================================================
// These are NOT clear/draw rects. They are the outer bounds of each logical
// zone so leaves can contribute to the per-frame DrawnRegion union without
// duplicating layout math. A leaf is free to add a tighter local rect if it
// knows its own precise FILL_RECT args — zone rects are the fallback.

// Primary content zone — frequency strip + arrow cluster share this Y range.
// x covers the full width of the frequency content area.
constexpr DisplayRect kFrequencyZoneRect{
    static_cast<int16_t>(CONTENT_LEFT_MARGIN),
    static_cast<int16_t>(PRIMARY_ZONE_Y),
    static_cast<int16_t>(CONTENT_AVAILABLE_WIDTH),
    static_cast<int16_t>(PRIMARY_ZONE_HEIGHT)
};

// Band-indicator column (left side).
constexpr DisplayRect kBandsColumnRect{
    0,
    static_cast<int16_t>(PRIMARY_ZONE_Y),
    static_cast<int16_t>(BAND_COLUMN_WIDTH),
    static_cast<int16_t>(PRIMARY_ZONE_HEIGHT)
};

// Signal-bars column (right side). Overapproximated to full signal column
// width — tight rect would hug just the bar glyphs but this keeps the math
// simple and union absorbs the extra width. Vertical extent matches the
// actual 8-bar stack (y=10..165, see drawVerticalSignalBars), which spans
// beyond the primary zone by design.
constexpr DisplayRect kSignalBarsRect{
    static_cast<int16_t>(SCREEN_WIDTH - SIGNAL_COLUMN_WIDTH),
    10,
    static_cast<int16_t>(SIGNAL_COLUMN_WIDTH),
    155
};

// Secondary alert-cards footer — full width overapproximated so we don't
// need to track which card slots are occupied for rect math.
constexpr DisplayRect kSecondaryCardsRect{
    0,
    static_cast<int16_t>(SECONDARY_ROW_Y),
    static_cast<int16_t>(SCREEN_WIDTH),
    static_cast<int16_t>(SECONDARY_ROW_HEIGHT)
};

// Top-counter field (top-left bogey counter glyph).
constexpr DisplayRect kTopCounterRect{
    static_cast<int16_t>(TOP_COUNTER_FIELD_X),
    static_cast<int16_t>(TOP_COUNTER_FIELD_Y),
    static_cast<int16_t>(TOP_COUNTER_FIELD_W),
    static_cast<int16_t>(TOP_COUNTER_FIELD_H)
};

// Renderer-owned element geometry. The visual-verification manifest exports
// these same helpers, so its assertions cannot drift away from the coordinates
// used to paint the framebuffer. Assertion probes are called out explicitly;
// all other rects describe the actual draw/clear boxes.

constexpr int BAND_CELL_COUNT = 4;

inline DisplayRect bandCellAssertRect(int index) {
    // FreeSansBold24pt7b glyph probes. These intentionally exclude the broader
    // per-cell clear windows because L overlaps the GPS badge and X overlaps
    // card slot 0. Keep these font-derived boxes paired with the renderer's
    // fixed x=82 and y=55+43*i anchors.
    const int clamped = (index < 0) ? 0 : ((index >= BAND_CELL_COUNT) ? (BAND_CELL_COUNT - 1) : index);
    switch (clamped) {
        case 0: return DisplayRect{82, 5, 23, 34};
        case 1: return DisplayRect{82, 48, 56, 35};
        case 2: return DisplayRect{82, 91, 30, 35};
        default: return DisplayRect{82, 134, 30, 34};
    }
}

constexpr int MAIN_SIGNAL_BAR_COUNT = 8;

inline DisplayRect mainSignalBarRect(int index) {
    constexpr int kBarWidth = 44;
    constexpr int kBarHeight = 15;
    constexpr int kBarSpacing = 5;
    constexpr int kStartX = SCREEN_WIDTH - SIGNAL_COLUMN_WIDTH;
    constexpr int kStartY = 10;
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
    // The historic row clear extends 8 px past slot 1 and stops 2 px before
    // the signal-bar lane. Preserve that wider cleanup window exactly.
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
    if (totalTop < 0) totalTop = 0;
    if (totalBottom > SCREEN_HEIGHT) totalBottom = SCREEN_HEIGHT;
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

// Convenience: effective screen height for primary zone rendering
inline int getEffectiveScreenHeight() {
    return DisplayLayout::PRIMARY_ZONE_HEIGHT;
}
