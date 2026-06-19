/**
 * test_display_rendering_bands.cpp
 *
 * Phase 3 Task 3.2 — integration tests for display_bands.cpp
 * (drawBandIndicators + drawVerticalSignalBars).
 *
 * Includes the real rendering source so that GFX call-recording
 * assertions on the injected Arduino_Canvas verify actual draw behaviour.
 */

#include <unity.h>

// ---------------------------------------------------------------------------
// Mocks (explicit relative path so the include guard fires before any real
// include/display_driver.h is pulled in by src/ headers)
// ---------------------------------------------------------------------------
#include "../mocks/display_driver.h"
#include "../mocks/Arduino.h"
#include "../mocks/settings.h"
#include "../mocks/packet_parser.h"

#ifndef ARDUINO
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
SerialClass Serial;
#endif

// Real display classes (display_driver.h guard already set to mock above)
#include "../../src/display.h"
#include "../../include/display_dirty_flags.h"
#include "../../include/display_element_caches.h"

// ---------------------------------------------------------------------------
// Required extern definitions
// ---------------------------------------------------------------------------
V1Display* g_displayInstance = nullptr;  // Set by V1Display constructor
SettingsManager settingsManager;

// ---------------------------------------------------------------------------
// Minimal V1Display constructor / destructor stubs
// (avoids pulling in all of display.cpp with its hardware dependencies)
// ---------------------------------------------------------------------------
V1Display::V1Display() {
    currentPalette_ = ColorThemes::STANDARD();
    currentPalette_.colorMuted = settingsManager.get().colorMuted;
    currentPalette_.colorPersisted = settingsManager.get().colorPersisted;
    g_displayInstance = this;
}
V1Display::~V1Display() = default;

// Global test display instance (owns the injected canvas via unique_ptr)
V1Display display;

// ---------------------------------------------------------------------------
// Real rendering code under test
// ---------------------------------------------------------------------------

// Stub: perf_metrics is not linked in rendering-only tests
#include "../../src/perf_metrics.h"
void perfRecordDisplayRedrawReason(PerfDisplayRedrawReason) {}

#include "../../src/display_bands.cpp"

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------
static Arduino_Canvas* canvas() { return display.testCanvas(); }

static void resetCanvas() {
    // Replace the canvas with a fresh one before each test.
    // V1Display takes ownership; old canvas is deleted by unique_ptr.
    display.setTestCanvas(new Arduino_Canvas(SCREEN_WIDTH, SCREEN_HEIGHT, nullptr));
    canvas()->resetCounters();
}

struct FontVisualBounds {
    int16_t width = 0;
    int16_t height = 0;
};

static FontVisualBounds freeSans24VisualBounds(const char* text) {
    const GFXfont& font = FreeSansBold24pt7b;
    int16_t minX = 0;
    int16_t minY = 0;
    int16_t maxX = 0;
    int16_t maxY = 0;
    int16_t cursorX = 0;
    bool seen = false;
    for (const char* p = text; *p; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        if (c < font.first || c > font.last) {
            continue;
        }
        const GFXglyph& glyph = font.glyph[c - font.first];
        const int16_t x0 = static_cast<int16_t>(cursorX + glyph.xOffset);
        const int16_t y0 = glyph.yOffset;
        const int16_t x1 = static_cast<int16_t>(x0 + glyph.width);
        const int16_t y1 = static_cast<int16_t>(y0 + glyph.height);
        if (!seen) {
            minX = x0;
            maxX = x1;
            minY = y0;
            maxY = y1;
            seen = true;
        } else {
            if (x0 < minX) minX = x0;
            if (x1 > maxX) maxX = x1;
            if (y0 < minY) minY = y0;
            if (y1 > maxY) maxY = y1;
        }
        cursorX = static_cast<int16_t>(cursorX + glyph.xAdvance);
    }
    return FontVisualBounds{
        static_cast<int16_t>(seen ? (maxX - minX) : 0),
        static_cast<int16_t>(seen ? (maxY - minY) : 0),
    };
}

// ---------------------------------------------------------------------------
// setUp / tearDown
// ---------------------------------------------------------------------------

void setUp() {
    mockMillis = 1000;
    resetCanvas();
    display.ut_elementCaches() = DisplayElementCaches{};
}

void tearDown() {}

// ============================================================================
// drawBandIndicators tests
// ============================================================================

void test_drawBandIndicators_produces_background_clear_on_first_draw() {
    display.ut_elementCaches().bands.valid = false;
    display.ut_drawBandIndicators(BAND_KA, false, 0);

    // Exactly one FILL_RECT clearing the entire band-label stack with PALETTE_BG
    TEST_ASSERT_EQUAL_UINT(1u, canvas()->fillRectCalls.size());
    TEST_ASSERT_EQUAL_UINT16(ColorThemes::STANDARD().bg, canvas()->fillRectCalls[0].color);
}

void test_band_label_dirty_window_covers_FreeSans_Ka_and_Ku_glyphs() {
    const FontVisualBounds ka = freeSans24VisualBounds("Ka");
    const FontVisualBounds ku = freeSans24VisualBounds("Ku");
    const int rightCoverageFromAnchor = kBandLabelClearW - kBandLabelClearLeftPad;

    TEST_ASSERT_GREATER_OR_EQUAL_INT(ka.width + 2, rightCoverageFromAnchor);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(ku.width + 2, rightCoverageFromAnchor);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(ka.height + 4, kBandLabelClearH);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(ku.height + 4, kBandLabelClearH);
}

void test_drawBandIndicators_cache_hit_skips_redraw() {
    display.ut_elementCaches().bands.valid = false;
    display.ut_drawBandIndicators(BAND_KA, false, 0);
    resetCanvas();

    // Second call with same args → no new draw calls
    display.ut_drawBandIndicators(BAND_KA, false, 0);
    TEST_ASSERT_EQUAL_UINT(0u, canvas()->fillRectCalls.size());
}

void test_drawBandIndicators_dirty_flag_forces_redraw() {
    display.ut_elementCaches().bands.valid = false;
    display.ut_drawBandIndicators(BAND_KA, false, 0);   // first draw (primes cache)
    resetCanvas();

    display.ut_elementCaches().bands.valid = false;   // invalidate cache
    display.ut_drawBandIndicators(BAND_KA, false, 0);

    // Cache was invalidated — expect at least one FILL_RECT
    TEST_ASSERT_GREATER_OR_EQUAL(1u, canvas()->fillRectCalls.size());
}

void test_drawBandIndicators_different_mask_invalidates_cache() {
    display.ut_elementCaches().bands.valid = false;
    display.ut_drawBandIndicators(BAND_KA, false, 0);
    resetCanvas();

    // Different band mask — cache miss → redraws
    display.ut_drawBandIndicators(BAND_KA | BAND_K, false, 0);
    TEST_ASSERT_EQUAL_UINT(1u, canvas()->fillRectCalls.size());
}

void test_drawBandIndicators_muted_change_invalidates_cache() {
    display.ut_elementCaches().bands.valid = false;
    display.ut_drawBandIndicators(BAND_KA, false, 0);
    resetCanvas();

    // Toggle muted → cache miss
    display.ut_drawBandIndicators(BAND_KA, true, 0);
    TEST_ASSERT_EQUAL_UINT(1u, canvas()->fillRectCalls.size());
}

// Drawing the K cell with the BAND_KU bit set must be
// treated as a different visual state from drawing K alone.  The cache key
// includes effectiveBandMask, so adding BAND_KU should miss the cache and
// trigger a fresh paint (where the K cell is relabelled "Ku").
void test_drawBandIndicators_ku_bit_invalidates_cache_vs_plain_k() {
    display.ut_elementCaches().bands.valid = false;
    display.ut_drawBandIndicators(BAND_K, false, 0);  // primes cache for plain K
    resetCanvas();

    display.ut_drawBandIndicators(static_cast<uint8_t>(BAND_K | BAND_KU), false, 0);
    TEST_ASSERT_EQUAL_UINT(1u, canvas()->fillRectCalls.size());
}

// And toggling Ku off must also re-invalidate.
void test_drawBandIndicators_clearing_ku_bit_invalidates_cache() {
    display.ut_elementCaches().bands.valid = false;
    display.ut_drawBandIndicators(static_cast<uint8_t>(BAND_K | BAND_KU), false, 0);
    resetCanvas();

    display.ut_drawBandIndicators(BAND_K, false, 0);
    TEST_ASSERT_EQUAL_UINT(1u, canvas()->fillRectCalls.size());
}

void test_drawBandIndicators_band_move_clears_only_changed_cells() {
    display.ut_elementCaches().bands.valid = false;
    display.ut_drawBandIndicators(BAND_LASER, false, 0);
    resetCanvas();

    display.ut_drawBandIndicators(BAND_K, false, 0);

    TEST_ASSERT_EQUAL_UINT(2u, canvas()->fillRectCalls.size());
    for (const auto& call : canvas()->fillRectCalls) {
        TEST_ASSERT_EQUAL_UINT16(ColorThemes::STANDARD().bg, call.color);
        TEST_ASSERT_EQUAL_INT16(kBandLabelClearW, call.w);
        TEST_ASSERT_EQUAL_INT16(kBandLabelClearH, call.h);
    }
}

void test_drawBandIndicators_ka_flash_redraws_full_stack_to_preserve_k() {
    const uint8_t kAndKa = static_cast<uint8_t>(BAND_KA | BAND_K);
    display.ut_setBlinkState(true, mockMillis);
    display.ut_elementCaches().bands.valid = false;
    display.ut_drawBandIndicators(kAndKa, false, BAND_KA);
    display.ut_resetDrawnRegion();
    resetCanvas();

    mockMillis += V1Display::getBlinkIntervalMs();
    display.ut_drawBandIndicators(kAndKa, false, BAND_KA);

    TEST_ASSERT_EQUAL_UINT(1u, canvas()->fillRectCalls.size());
    TEST_ASSERT_EQUAL_INT16(kBandLabelClearW, canvas()->fillRectCalls[0].w);
    TEST_ASSERT_GREATER_THAN_INT16(kBandLabelClearH, canvas()->fillRectCalls[0].h);
    TEST_ASSERT_FALSE(display.ut_drawnRegionEmpty());
    TEST_ASSERT_EQUAL_INT(kBandLabelClearW, display.ut_drawnRegionW());
    TEST_ASSERT_GREATER_THAN_INT(kBandLabelClearH, display.ut_drawnRegionH());
}

void test_drawBandIndicators_inactive_muted_toggle_skips_visual_redraw() {
    display.ut_elementCaches().bands.valid = false;
    display.ut_drawBandIndicators(0, false, 0);
    resetCanvas();

    display.ut_drawBandIndicators(0, true, 0);

    TEST_ASSERT_EQUAL_UINT(0u, canvas()->fillRectCalls.size());
}

// ============================================================================
// drawVerticalSignalBars tests
// ============================================================================

// Helper: force signal bars redraw and return fillRoundRectCalls count
static size_t signalBarsRedrawCount(uint8_t front, uint8_t rear, bool muted) {
    display.ut_elementCaches().bars.valid = false;
    resetCanvas();
    display.ut_drawVerticalSignalBars(front, rear, BAND_KA, muted);
    return canvas()->fillRoundRectCalls.size();
}

void test_drawSignalBars_strength_4_draws_6_bars() {
    TEST_ASSERT_EQUAL_UINT(6u, signalBarsRedrawCount(4, 0, false));
}

void test_drawSignalBars_strength_0_draws_6_unlit_bars() {
    // strength=0 → all 6 bars are unlit and need drawing (first render after cache clear)
    TEST_ASSERT_EQUAL_UINT(6u, signalBarsRedrawCount(0, 0, false));
}

void test_drawSignalBars_lit_bars_use_bar_colors() {
    display.ut_elementCaches().bars.valid = false;
    resetCanvas();
    display.ut_drawVerticalSignalBars(4, 0, BAND_KA, false);

    const auto& calls = canvas()->fillRoundRectCalls;
    TEST_ASSERT_EQUAL_UINT(6u, calls.size());

    // Bars 0-3 (i < strength=4) must use the configured barN colors
    const V1Settings& s = settingsManager.get();
    TEST_ASSERT_EQUAL_UINT16(s.colorBar1, calls[0].color);
    TEST_ASSERT_EQUAL_UINT16(s.colorBar2, calls[1].color);
    TEST_ASSERT_EQUAL_UINT16(s.colorBar3, calls[2].color);
    TEST_ASSERT_EQUAL_UINT16(s.colorBar4, calls[3].color);
}

void test_drawSignalBars_unlit_bars_use_dark_gray() {
    display.ut_elementCaches().bars.valid = false;
    resetCanvas();
    display.ut_drawVerticalSignalBars(4, 0, BAND_KA, false);

    const auto& calls = canvas()->fillRoundRectCalls;
    // Bars 4 and 5 (past strength) must use 0x1082 (off-color)
    TEST_ASSERT_EQUAL_UINT16(0x1082, calls[4].color);
    TEST_ASSERT_EQUAL_UINT16(0x1082, calls[5].color);
}

void test_drawSignalBars_muted_uses_muted_color() {
    display.ut_elementCaches().bars.valid = false;
    resetCanvas();
    display.ut_drawVerticalSignalBars(4, 0, BAND_KA, /*muted=*/true);

    const auto& calls = canvas()->fillRoundRectCalls;
    TEST_ASSERT_EQUAL_UINT(6u, calls.size());

    // All lit bars (i < 4) must use PALETTE_MUTED = colorMuted
    const uint16_t expectedMuted = settingsManager.get().colorMuted;
    TEST_ASSERT_EQUAL_UINT16(expectedMuted, calls[0].color);
    TEST_ASSERT_EQUAL_UINT16(expectedMuted, calls[1].color);
    TEST_ASSERT_EQUAL_UINT16(expectedMuted, calls[2].color);
    TEST_ASSERT_EQUAL_UINT16(expectedMuted, calls[3].color);
    // Unlit bars still use 0x1082
    TEST_ASSERT_EQUAL_UINT16(0x1082, calls[4].color);
    TEST_ASSERT_EQUAL_UINT16(0x1082, calls[5].color);
}

void test_drawSignalBars_cache_hit_no_redraw() {
    display.ut_elementCaches().bars.valid = false;
    display.ut_drawVerticalSignalBars(4, 0, BAND_KA, false);   // primes cache
    resetCanvas();

    // Same args → cache hit, no new calls
    display.ut_drawVerticalSignalBars(4, 0, BAND_KA, false);
    TEST_ASSERT_EQUAL_UINT(0u, canvas()->fillRoundRectCalls.size());
}

void test_drawSignalBars_dirty_flag_forces_redraw() {
    display.ut_elementCaches().bars.valid = false;
    display.ut_drawVerticalSignalBars(4, 0, BAND_KA, false);   // first draw
    resetCanvas();

    display.ut_elementCaches().bars.valid = false;     // invalidate
    display.ut_drawVerticalSignalBars(4, 0, BAND_KA, false);

    TEST_ASSERT_EQUAL_UINT(6u, canvas()->fillRoundRectCalls.size());
}

void test_drawSignalBars_max_of_front_rear_used() {
    // rearStrength > frontStrength → max is used
    display.ut_elementCaches().bars.valid = false;
    resetCanvas();
    display.ut_drawVerticalSignalBars(2, 5, BAND_KA, false);

    // 5 lit + 1 unlit = 6 bars. Bars 0-4 use bar colors, bar 5 uses 0x1082.
    const auto& calls = canvas()->fillRoundRectCalls;
    TEST_ASSERT_EQUAL_UINT(6u, calls.size());
    const V1Settings& s = settingsManager.get();
    TEST_ASSERT_EQUAL_UINT16(s.colorBar5, calls[4].color);   // i=4 → 5th bar from bottom
    TEST_ASSERT_EQUAL_UINT16(0x1082, calls[5].color);        // i=5 → unlit
}

void test_drawSignalBars_strength_above_6_lights_all_6_bars() {
    display.ut_elementCaches().bars.valid = false;
    resetCanvas();
    display.ut_drawVerticalSignalBars(8, 0, BAND_KA, false);

    const auto& calls = canvas()->fillRoundRectCalls;
    TEST_ASSERT_EQUAL_UINT(6u, calls.size());
    const V1Settings& s = settingsManager.get();
    TEST_ASSERT_EQUAL_UINT16(s.colorBar1, calls[0].color);
    TEST_ASSERT_EQUAL_UINT16(s.colorBar2, calls[1].color);
    TEST_ASSERT_EQUAL_UINT16(s.colorBar3, calls[2].color);
    TEST_ASSERT_EQUAL_UINT16(s.colorBar4, calls[3].color);
    TEST_ASSERT_EQUAL_UINT16(s.colorBar5, calls[4].color);
    TEST_ASSERT_EQUAL_UINT16(s.colorBar6, calls[5].color);
}

void test_drawSignalBars_marks_full_stack_dirty_region() {
    display.ut_elementCaches().bars.valid = false;
    display.ut_resetDrawnRegion();
    resetCanvas();
    display.ut_drawVerticalSignalBars(8, 0, BAND_KA, false);

    TEST_ASSERT_FALSE(display.ut_drawnRegionEmpty());
    TEST_ASSERT_EQUAL_INT(SCREEN_WIDTH - 200, display.ut_drawnRegionX());
    TEST_ASSERT_EQUAL_INT(18, display.ut_drawnRegionY());
    TEST_ASSERT_EQUAL_INT(44, display.ut_drawnRegionW());
    TEST_ASSERT_EQUAL_INT(6 * (14 + 10) - 10, display.ut_drawnRegionH());
}

void test_drawSignalBars_strength_increment_marks_only_changed_bar_region() {
    display.ut_elementCaches().bars.valid = false;
    display.ut_drawVerticalSignalBars(4, 0, BAND_KA, false);
    display.ut_resetDrawnRegion();
    resetCanvas();

    display.ut_drawVerticalSignalBars(5, 0, BAND_KA, false);

    TEST_ASSERT_EQUAL_UINT(1u, canvas()->fillRoundRectCalls.size());
    TEST_ASSERT_FALSE(display.ut_drawnRegionEmpty());
    TEST_ASSERT_EQUAL_INT(SCREEN_WIDTH - 200, display.ut_drawnRegionX());
    TEST_ASSERT_EQUAL_INT(18 + (14 + 10), display.ut_drawnRegionY());
    TEST_ASSERT_EQUAL_INT(44, display.ut_drawnRegionW());
    TEST_ASSERT_EQUAL_INT(14, display.ut_drawnRegionH());
    TEST_ASSERT_EQUAL_UINT8(1u, display.ut_drawnRegionRectCount());
}

void test_drawSignalBars_strength_jump_marks_one_compact_run_region() {
    display.ut_elementCaches().bars.valid = false;
    display.ut_drawVerticalSignalBars(2, 0, BAND_KA, false);
    display.ut_resetDrawnRegion();
    resetCanvas();

    display.ut_drawVerticalSignalBars(6, 0, BAND_KA, false);

    TEST_ASSERT_EQUAL_UINT(4u, canvas()->fillRoundRectCalls.size());
    TEST_ASSERT_FALSE(display.ut_drawnRegionEmpty());
    TEST_ASSERT_EQUAL_INT(SCREEN_WIDTH - 200, display.ut_drawnRegionX());
    TEST_ASSERT_EQUAL_INT(18, display.ut_drawnRegionY());
    TEST_ASSERT_EQUAL_INT(44, display.ut_drawnRegionW());
    TEST_ASSERT_EQUAL_INT((4 * 14) + (3 * 10), display.ut_drawnRegionH());
    TEST_ASSERT_EQUAL_UINT8(1u, display.ut_drawnRegionRectCount());
}

// ============================================================================
// main
// ============================================================================

int main() {
    UNITY_BEGIN();

    // drawBandIndicators
    RUN_TEST(test_drawBandIndicators_produces_background_clear_on_first_draw);
    RUN_TEST(test_band_label_dirty_window_covers_FreeSans_Ka_and_Ku_glyphs);
    RUN_TEST(test_drawBandIndicators_cache_hit_skips_redraw);
    RUN_TEST(test_drawBandIndicators_dirty_flag_forces_redraw);
    RUN_TEST(test_drawBandIndicators_different_mask_invalidates_cache);
    RUN_TEST(test_drawBandIndicators_muted_change_invalidates_cache);
    RUN_TEST(test_drawBandIndicators_ku_bit_invalidates_cache_vs_plain_k);
    RUN_TEST(test_drawBandIndicators_clearing_ku_bit_invalidates_cache);
    RUN_TEST(test_drawBandIndicators_band_move_clears_only_changed_cells);
    RUN_TEST(test_drawBandIndicators_ka_flash_redraws_full_stack_to_preserve_k);
    RUN_TEST(test_drawBandIndicators_inactive_muted_toggle_skips_visual_redraw);

    // drawVerticalSignalBars
    RUN_TEST(test_drawSignalBars_strength_4_draws_6_bars);
    RUN_TEST(test_drawSignalBars_strength_0_draws_6_unlit_bars);
    RUN_TEST(test_drawSignalBars_lit_bars_use_bar_colors);
    RUN_TEST(test_drawSignalBars_unlit_bars_use_dark_gray);
    RUN_TEST(test_drawSignalBars_muted_uses_muted_color);
    RUN_TEST(test_drawSignalBars_cache_hit_no_redraw);
    RUN_TEST(test_drawSignalBars_dirty_flag_forces_redraw);
    RUN_TEST(test_drawSignalBars_max_of_front_rear_used);
    RUN_TEST(test_drawSignalBars_strength_above_6_lights_all_6_bars);
    RUN_TEST(test_drawSignalBars_marks_full_stack_dirty_region);
    RUN_TEST(test_drawSignalBars_strength_increment_marks_only_changed_bar_region);
    RUN_TEST(test_drawSignalBars_strength_jump_marks_one_compact_run_region);

    return UNITY_END();
}
