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

// Expected color math for the 8-position meter: the six configured bar
// colors are sampled across eight positions with linear RGB565 blending
// (bar i → continuous index i*5/7). This mirrors the renderer's mapping so
// the gradient contract is pinned by test, not just by implementation.
static uint16_t lerp565(uint16_t a, uint16_t b, uint8_t num, uint8_t den) {
    const int ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
    const int br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
    const int r = ar + (((br - ar) * num) + den / 2) / den;
    const int g = ag + (((bg - ag) * num) + den / 2) / den;
    const int bl = ab + (((bb - ab) * num) + den / 2) / den;
    return static_cast<uint16_t>((r << 11) | (g << 5) | bl);
}

static uint16_t expectedBarColor(int i) {
    const V1Settings& s = settingsManager.get();
    const uint16_t c[6] = {s.colorBar1, s.colorBar2, s.colorBar3,
                           s.colorBar4, s.colorBar5, s.colorBar6};
    const int scaled = i * 5;
    const int idx = scaled / 7;
    const int rem = scaled % 7;
    return (rem == 0 || idx >= 5)
               ? c[idx]
               : lerp565(c[idx], c[idx + 1], static_cast<uint8_t>(rem), 7);
}

void test_drawSignalBars_strength_4_draws_8_bars() {
    TEST_ASSERT_EQUAL_UINT(8u, signalBarsRedrawCount(4, 0, false));
}

void test_drawSignalBars_strength_0_draws_8_unlit_bars() {
    // strength=0 → all 8 bars are unlit and need drawing (first render after cache clear)
    TEST_ASSERT_EQUAL_UINT(8u, signalBarsRedrawCount(0, 0, false));
}

void test_drawSignalBars_lit_bars_use_interpolated_bar_colors() {
    display.ut_elementCaches().bars.valid = false;
    resetCanvas();
    display.ut_drawVerticalSignalBars(4, 0, BAND_KA, false);

    const auto& calls = canvas()->fillRoundRectCalls;
    TEST_ASSERT_EQUAL_UINT(8u, calls.size());

    // Bars 0-3 (i < strength=4) sample the configured color ramp; endpoints
    // land exactly on colorBar1 (i=0) and colorBar6 (i=7).
    const V1Settings& s = settingsManager.get();
    TEST_ASSERT_EQUAL_UINT16(s.colorBar1, expectedBarColor(0));
    TEST_ASSERT_EQUAL_UINT16(s.colorBar6, expectedBarColor(7));
    for (int i = 0; i < 4; ++i) {
        TEST_ASSERT_EQUAL_UINT16(expectedBarColor(i), calls[i].color);
    }
}

void test_drawSignalBars_unlit_bars_use_dark_gray() {
    display.ut_elementCaches().bars.valid = false;
    resetCanvas();
    display.ut_drawVerticalSignalBars(4, 0, BAND_KA, false);

    const auto& calls = canvas()->fillRoundRectCalls;
    // Bars 4-7 (past strength) must use 0x1082 (off-color)
    for (int i = 4; i < 8; ++i) {
        TEST_ASSERT_EQUAL_UINT16(0x1082, calls[i].color);
    }
}

void test_drawSignalBars_muted_uses_muted_color() {
    display.ut_elementCaches().bars.valid = false;
    resetCanvas();
    display.ut_drawVerticalSignalBars(4, 0, BAND_KA, /*muted=*/true);

    const auto& calls = canvas()->fillRoundRectCalls;
    TEST_ASSERT_EQUAL_UINT(8u, calls.size());

    // All lit bars (i < 4) must use PALETTE_MUTED = colorMuted
    const uint16_t expectedMuted = settingsManager.get().colorMuted;
    for (int i = 0; i < 4; ++i) {
        TEST_ASSERT_EQUAL_UINT16(expectedMuted, calls[i].color);
    }
    // Unlit bars still use 0x1082
    for (int i = 4; i < 8; ++i) {
        TEST_ASSERT_EQUAL_UINT16(0x1082, calls[i].color);
    }
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

    TEST_ASSERT_EQUAL_UINT(8u, canvas()->fillRoundRectCalls.size());
}

void test_drawSignalBars_max_of_front_rear_used() {
    // rearStrength > frontStrength → max is used
    display.ut_elementCaches().bars.valid = false;
    resetCanvas();
    display.ut_drawVerticalSignalBars(2, 5, BAND_KA, false);

    // 5 lit + 3 unlit = 8 bars. Bars 0-4 use ramp colors, bars 5-7 use 0x1082.
    const auto& calls = canvas()->fillRoundRectCalls;
    TEST_ASSERT_EQUAL_UINT(8u, calls.size());
    TEST_ASSERT_EQUAL_UINT16(expectedBarColor(4), calls[4].color);  // i=4 → 5th bar from bottom
    TEST_ASSERT_EQUAL_UINT16(0x1082, calls[5].color);               // i=5 → unlit
    TEST_ASSERT_EQUAL_UINT16(0x1082, calls[7].color);               // i=7 → unlit
}

// Full-scale mirror: V1 strength 8 must light all eight bars, including the
// top one. This is also the critical-011 mutation killer (a >= clamp would
// drop the exact-max strength to 7 and leave calls[7] unlit).
void test_drawSignalBars_strength_8_lights_all_8_bars() {
    display.ut_elementCaches().bars.valid = false;
    resetCanvas();
    display.ut_drawVerticalSignalBars(8, 0, BAND_KA, false);

    const auto& calls = canvas()->fillRoundRectCalls;
    TEST_ASSERT_EQUAL_UINT(8u, calls.size());
    for (int i = 0; i < 8; ++i) {
        TEST_ASSERT_EQUAL_UINT16(expectedBarColor(i), calls[i].color);
    }
    const V1Settings& s = settingsManager.get();
    TEST_ASSERT_EQUAL_UINT16(s.colorBar1, calls[0].color);
    TEST_ASSERT_EQUAL_UINT16(s.colorBar6, calls[7].color);
}

void test_drawSignalBars_strength_above_8_clamps_to_all_8_bars() {
    display.ut_elementCaches().bars.valid = false;
    resetCanvas();
    display.ut_drawVerticalSignalBars(9, 0, BAND_KA, false);

    const auto& calls = canvas()->fillRoundRectCalls;
    TEST_ASSERT_EQUAL_UINT(8u, calls.size());
    TEST_ASSERT_EQUAL_UINT16(expectedBarColor(7), calls[7].color);  // top bar lit
}

void test_drawSignalBars_marks_full_stack_dirty_region() {
    display.ut_elementCaches().bars.valid = false;
    display.ut_resetDrawnRegion();
    resetCanvas();
    display.ut_drawVerticalSignalBars(8, 0, BAND_KA, false);

    TEST_ASSERT_FALSE(display.ut_drawnRegionEmpty());
    TEST_ASSERT_EQUAL_INT(SCREEN_WIDTH - 200, display.ut_drawnRegionX());
    TEST_ASSERT_EQUAL_INT(10, display.ut_drawnRegionY());
    TEST_ASSERT_EQUAL_INT(44, display.ut_drawnRegionW());
    TEST_ASSERT_EQUAL_INT(8 * (15 + 5) - 5, display.ut_drawnRegionH());
}

void test_drawSignalBars_strength_increment_marks_only_changed_bar_region() {
    display.ut_elementCaches().bars.valid = false;
    display.ut_drawVerticalSignalBars(4, 0, BAND_KA, false);
    display.ut_resetDrawnRegion();
    resetCanvas();

    display.ut_drawVerticalSignalBars(5, 0, BAND_KA, false);

    // Bar i=4 changed; visual row = 8-1-4 = 3 → y = 10 + 3*(15+5).
    TEST_ASSERT_EQUAL_UINT(1u, canvas()->fillRoundRectCalls.size());
    TEST_ASSERT_FALSE(display.ut_drawnRegionEmpty());
    TEST_ASSERT_EQUAL_INT(SCREEN_WIDTH - 200, display.ut_drawnRegionX());
    TEST_ASSERT_EQUAL_INT(10 + 3 * (15 + 5), display.ut_drawnRegionY());
    TEST_ASSERT_EQUAL_INT(44, display.ut_drawnRegionW());
    TEST_ASSERT_EQUAL_INT(15, display.ut_drawnRegionH());
    TEST_ASSERT_EQUAL_UINT8(1u, display.ut_drawnRegionRectCount());
}

void test_drawSignalBars_strength_jump_marks_one_compact_run_region() {
    display.ut_elementCaches().bars.valid = false;
    display.ut_drawVerticalSignalBars(2, 0, BAND_KA, false);
    display.ut_resetDrawnRegion();
    resetCanvas();

    display.ut_drawVerticalSignalBars(6, 0, BAND_KA, false);

    // Bars i=2..5 changed; visual rows 5..2 → y from 10+2*20 spanning 4 bars.
    TEST_ASSERT_EQUAL_UINT(4u, canvas()->fillRoundRectCalls.size());
    TEST_ASSERT_FALSE(display.ut_drawnRegionEmpty());
    TEST_ASSERT_EQUAL_INT(SCREEN_WIDTH - 200, display.ut_drawnRegionX());
    TEST_ASSERT_EQUAL_INT(10 + 2 * (15 + 5), display.ut_drawnRegionY());
    TEST_ASSERT_EQUAL_INT(44, display.ut_drawnRegionW());
    TEST_ASSERT_EQUAL_INT((4 * 15) + (3 * 5), display.ut_drawnRegionH());
    TEST_ASSERT_EQUAL_UINT8(1u, display.ut_drawnRegionRectCount());
}

// Valentine's Law (the display must not lie by going stale): the bars/bands
// caches key the palette revision, so a color-theme change repaints even when
// strength/mask/muted are unchanged — matching every sibling cache.
void test_drawSignalBars_palette_revision_change_forces_repaint() {
    display.ut_elementCaches().bars.valid = false;
    display.ut_drawVerticalSignalBars(4, 0, BAND_KA, false);   // prime cache
    resetCanvas();

    display.ut_drawVerticalSignalBars(4, 0, BAND_KA, false);   // cache hit
    TEST_ASSERT_EQUAL_UINT(0u, canvas()->fillRoundRectCalls.size());

    display.ut_bumpPaletteRevision();                          // theme change signal
    display.ut_drawVerticalSignalBars(4, 0, BAND_KA, false);   // must repaint
    TEST_ASSERT_EQUAL_UINT(8u, canvas()->fillRoundRectCalls.size());
}

void test_drawBandIndicators_palette_revision_change_forces_repaint() {
    display.ut_elementCaches().bands.valid = false;
    display.ut_drawBandIndicators(BAND_KA, false, 0);          // prime cache
    resetCanvas();

    TEST_ASSERT_FALSE(display.ut_drawBandIndicators(BAND_KA, false, 0));  // cache hit

    display.ut_bumpPaletteRevision();                          // theme change signal
    TEST_ASSERT_TRUE(display.ut_drawBandIndicators(BAND_KA, false, 0));   // must repaint
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
    RUN_TEST(test_drawSignalBars_strength_4_draws_8_bars);
    RUN_TEST(test_drawSignalBars_strength_0_draws_8_unlit_bars);
    RUN_TEST(test_drawSignalBars_lit_bars_use_interpolated_bar_colors);
    RUN_TEST(test_drawSignalBars_unlit_bars_use_dark_gray);
    RUN_TEST(test_drawSignalBars_muted_uses_muted_color);
    RUN_TEST(test_drawSignalBars_cache_hit_no_redraw);
    RUN_TEST(test_drawSignalBars_dirty_flag_forces_redraw);
    RUN_TEST(test_drawSignalBars_max_of_front_rear_used);
    RUN_TEST(test_drawSignalBars_strength_8_lights_all_8_bars);
    RUN_TEST(test_drawSignalBars_strength_above_8_clamps_to_all_8_bars);
    RUN_TEST(test_drawSignalBars_marks_full_stack_dirty_region);
    RUN_TEST(test_drawSignalBars_strength_increment_marks_only_changed_bar_region);
    RUN_TEST(test_drawSignalBars_strength_jump_marks_one_compact_run_region);
    RUN_TEST(test_drawSignalBars_palette_revision_change_forces_repaint);
    RUN_TEST(test_drawBandIndicators_palette_revision_change_forces_repaint);

    return UNITY_END();
}
