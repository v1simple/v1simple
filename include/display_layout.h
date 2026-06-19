/**
 * Display Layout Constants for V1 Gen2 Display
 * Waveshare ESP32-S3-Touch-LCD-3.49 (640x172 LCD)
 *
 * Centralizes all layout-related constants to ensure consistency
 * across display.cpp and tests. Derived from config.h screen dimensions.
 *
 * Layout Overview (640x172 landscape):
 * ┌────────────────────────────────────────────────────────────────────────────┐
 * │ [Status Bar: indicators]                                      [WiFi]      │ Y=0-20
 * ├──────────┬───────────────────────────────────────────┬────────────────────┤
 * │          │                                           │  Signal Bars       │
 * │  Band    │   Frequency / Alert Content               │  Direction Arrow   │ Y=20-95
 * │  Labels  │   (frequency can extend to screen bottom) │  Volume/RSSI       │
 * │          │                                           │                    │
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

// Bottom boundary for primary content that may extend below the fixed primary
// zone when alert text or preview labels need the reclaimed footer space.
constexpr int CONTENT_BOTTOM_Y = SCREEN_HEIGHT;

// Verify zones don't overlap (compile-time check)
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
// simple and union absorbs the extra width.
constexpr DisplayRect kSignalBarsRect{
    static_cast<int16_t>(SCREEN_WIDTH - SIGNAL_COLUMN_WIDTH),
    static_cast<int16_t>(PRIMARY_ZONE_Y),
    static_cast<int16_t>(SIGNAL_COLUMN_WIDTH),
    static_cast<int16_t>(PRIMARY_ZONE_HEIGHT)
};

// Top-counter field (top-left bogey counter glyph).
constexpr DisplayRect kTopCounterRect{
    static_cast<int16_t>(TOP_COUNTER_FIELD_X),
    static_cast<int16_t>(TOP_COUNTER_FIELD_Y),
    static_cast<int16_t>(TOP_COUNTER_FIELD_W),
    static_cast<int16_t>(TOP_COUNTER_FIELD_H)
};

} // namespace DisplayLayout

// Convenience: effective screen height for primary zone rendering
inline int getEffectiveScreenHeight() {
    return DisplayLayout::PRIMARY_ZONE_HEIGHT;
}
