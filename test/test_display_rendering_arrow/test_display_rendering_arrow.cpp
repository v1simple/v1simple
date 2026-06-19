/**
 * test_display_rendering_arrow.cpp
 *
 * Phase 3 Task 3.3 — integration tests for display_arrow.cpp
 * (drawDirectionArrow).
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

// Stub: drawBatteryIndicator lives in display_status_bar.cpp; the D7 fix in
// drawDirectionArrow calls it after a raised-layout cluster paint to restore
// the battery icon. We stub it here so the rendering-only test keeps a clean
// link surface — battery rendering has its own dedicated tests.
void V1Display::drawBatteryIndicator() {}

#include "../../src/display_arrow.cpp"

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------
static Arduino_Canvas* canvas() { return display.testCanvas(); }

static void resetCanvas() {
    display.setTestCanvas(new Arduino_Canvas(SCREEN_WIDTH, SCREEN_HEIGHT, nullptr));
    canvas()->resetCounters();
}

// Arrow active direction combinations
static const Direction DIR_ALL = static_cast<Direction>(DIR_FRONT | DIR_SIDE | DIR_REAR);

// Default arrow colors (from settings mock)
static constexpr uint16_t FRONT_COL = 0xF800;  // s.colorArrowFront default
static constexpr uint16_t SIDE_COL  = 0xF800;  // s.colorArrowSide default
static constexpr uint16_t REAR_COL  = 0xF800;  // s.colorArrowRear default
static constexpr uint16_t OFF_COL   = 0x1082;  // dim resting arrows (TFT_DARKGREY)
static constexpr uint16_t MUTED_COL = 0x8410;  // colorMuted default

// ---------------------------------------------------------------------------
// setUp / tearDown
// ---------------------------------------------------------------------------

void setUp() {
    mockMillis = 1000;
    resetCanvas();
    display.ut_elementCaches() = DisplayElementCaches{};  // invalidate all caches every test
    display.ut_setBlinkState(true, mockMillis);
}

void tearDown() {}

// ============================================================================
// Full-redraw structure tests (cacheValid forced to false via elementCaches_.arrow.valid)
// ============================================================================

void test_drawArrow_full_redraw_produces_4_fill_triangles() {
    // Full redraw draws: 1 top triangle + 2 side triangles + 1 rear triangle
    display.ut_drawDirectionArrow(DIR_ALL, false, 0, 0);
    TEST_ASSERT_EQUAL_UINT(4u, canvas()->fillTriangleCalls.size());
}

void test_drawArrow_full_redraw_clears_region_with_bg_color() {
    display.ut_drawDirectionArrow(DIR_ALL, false, 0, 0);
    // First fillRect call is the full-region clear with PALETTE_BG
    TEST_ASSERT_GREATER_OR_EQUAL(1u, canvas()->fillRectCalls.size());
    TEST_ASSERT_EQUAL_UINT16(ColorThemes::STANDARD().bg, canvas()->fillRectCalls[0].color);
}

// ============================================================================
// Cache tests
// ============================================================================

void test_drawArrow_cache_hit_skips_redraw() {
    // First draw primes the cache
    display.ut_drawDirectionArrow(DIR_ALL, false, 0, 0);
    resetCanvas();

    // Same args, no dirty flag → return early, no new draw calls
    display.ut_drawDirectionArrow(DIR_ALL, false, 0, 0);
    TEST_ASSERT_EQUAL_UINT(0u, canvas()->fillTriangleCalls.size());
}

void test_drawArrow_dirty_arrow_flag_forces_redraw() {
    display.ut_drawDirectionArrow(DIR_ALL, false, 0, 0);  // primes cache
    resetCanvas();

    display.ut_elementCaches().arrow.valid = false;  // invalidate
    display.ut_drawDirectionArrow(DIR_ALL, false, 0, 0);
    TEST_ASSERT_EQUAL_UINT(4u, canvas()->fillTriangleCalls.size());
}

void test_drawArrow_direction_change_invalidates_cache() {
    // Prime cache with no arrows active
    display.ut_drawDirectionArrow(DIR_NONE, false, 0, 0);
    resetCanvas();

    // All three visibility bits change at once → full redraw (4 triangles)
    display.ut_drawDirectionArrow(DIR_ALL, false, 0, 0);
    TEST_ASSERT_EQUAL_UINT(4u, canvas()->fillTriangleCalls.size());
}

void test_drawArrow_muted_change_invalidates_cache() {
    display.ut_drawDirectionArrow(DIR_ALL, false, 0, 0);  // primes cache (not muted)
    resetCanvas();

    // Muted changes → cache miss → redraws
    display.ut_drawDirectionArrow(DIR_ALL, true, 0, 0);
    TEST_ASSERT_EQUAL_UINT(4u, canvas()->fillTriangleCalls.size());
}

// ============================================================================
// Active-color tests (fillTriangleCalls[0]=top, [1]=side-left, [2]=side-right, [3]=rear)
// ============================================================================

void test_drawArrow_front_only_top_triangle_uses_front_color() {
    display.ut_drawDirectionArrow(DIR_FRONT, false, 0, 0);
    auto& tc = canvas()->fillTriangleCalls;
    TEST_ASSERT_EQUAL_UINT(4u, tc.size());
    TEST_ASSERT_EQUAL_UINT16(FRONT_COL, tc[0].color);  // top   = active
    TEST_ASSERT_EQUAL_UINT16(OFF_COL,   tc[1].color);  // side  = resting
    TEST_ASSERT_EQUAL_UINT16(OFF_COL,   tc[2].color);  // side  = resting
    TEST_ASSERT_EQUAL_UINT16(OFF_COL,   tc[3].color);  // rear  = resting
}

void test_drawArrow_rear_only_bottom_triangle_uses_rear_color() {
    display.ut_drawDirectionArrow(DIR_REAR, false, 0, 0);
    auto& tc = canvas()->fillTriangleCalls;
    TEST_ASSERT_EQUAL_UINT(4u, tc.size());
    TEST_ASSERT_EQUAL_UINT16(OFF_COL,  tc[0].color);  // top  = resting
    TEST_ASSERT_EQUAL_UINT16(OFF_COL,  tc[1].color);  // side = resting
    TEST_ASSERT_EQUAL_UINT16(OFF_COL,  tc[2].color);  // side = resting
    TEST_ASSERT_EQUAL_UINT16(REAR_COL, tc[3].color);  // rear = active
}

void test_drawArrow_side_only_both_side_triangles_use_side_color() {
    display.ut_drawDirectionArrow(DIR_SIDE, false, 0, 0);
    auto& tc = canvas()->fillTriangleCalls;
    TEST_ASSERT_EQUAL_UINT(4u, tc.size());
    TEST_ASSERT_EQUAL_UINT16(OFF_COL,  tc[0].color);  // top  = resting
    TEST_ASSERT_EQUAL_UINT16(SIDE_COL, tc[1].color);  // side-left  = active
    TEST_ASSERT_EQUAL_UINT16(SIDE_COL, tc[2].color);  // side-right = active
    TEST_ASSERT_EQUAL_UINT16(OFF_COL,  tc[3].color);  // rear = resting
}

void test_drawArrow_muted_all_triangles_use_muted_color() {
    display.ut_drawDirectionArrow(DIR_ALL, true, 0, 0);
    auto& tc = canvas()->fillTriangleCalls;
    TEST_ASSERT_EQUAL_UINT(4u, tc.size());
    for (size_t i = 0; i < tc.size(); ++i) {
        TEST_ASSERT_EQUAL_UINT16_MESSAGE(MUTED_COL, tc[i].color, "all arrows should use muted color");
    }
}

void test_drawArrow_no_arrows_all_triangles_use_off_color() {
    display.ut_drawDirectionArrow(DIR_NONE, false, 0, 0);
    auto& tc = canvas()->fillTriangleCalls;
    TEST_ASSERT_EQUAL_UINT(4u, tc.size());
    for (size_t i = 0; i < tc.size(); ++i) {
        TEST_ASSERT_EQUAL_UINT16_MESSAGE(OFF_COL, tc[i].color, "all arrows should use off color");
    }
}

void test_drawArrow_flash_bit_waits_full_96ms_before_toggling() {
    // V1 protocol summary: docs/V1_PROTOCOL_REFERENCES.md#infdisplaydata.
    // The V1 blink cadence is a 96 ms toggle. See display.h BLINK_INTERVAL_MS.
    display.ut_drawDirectionArrow(DIR_FRONT, false, 0x20, 0);
    TEST_ASSERT_EQUAL_UINT(4u, canvas()->fillTriangleCalls.size());
    TEST_ASSERT_EQUAL_UINT16(FRONT_COL, canvas()->fillTriangleCalls[0].color);

    resetCanvas();
    mockMillis = 1095;
    display.ut_drawDirectionArrow(DIR_FRONT, false, 0x20, 0);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0u, canvas()->fillTriangleCalls.size(),
                                   "blink phase must not toggle before 96ms");

    resetCanvas();
    mockMillis = 1096;
    display.ut_drawDirectionArrow(DIR_FRONT, false, 0x20, 0);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT_MESSAGE(1u, canvas()->fillTriangleCalls.size(),
                                              "blink phase must toggle at 96ms");
    // Blink-off must erase to PALETTE_BG so the panel toggle is actually
    // visible and matches V1 Image2.
    TEST_ASSERT_EQUAL_UINT16(0x0000 /*PALETTE_BG*/, canvas()->fillTriangleCalls[0].color);
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(OFF_COL, canvas()->fillTriangleCalls[1].color,
        "non-active side arrow should remain a dim resting glyph during front blink-off");
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(OFF_COL, canvas()->fillTriangleCalls[2].color,
        "non-active side arrow should remain a dim resting glyph during front blink-off");
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(OFF_COL, canvas()->fillTriangleCalls[3].color,
        "non-active rear arrow should remain a dim resting glyph during front blink-off");
}

void test_drawArrow_late_by_two_intervals_keeps_v1_phase() {
    display.ut_drawDirectionArrow(DIR_FRONT, false, 0x20, 0);
    TEST_ASSERT_EQUAL_UINT16(FRONT_COL, canvas()->fillTriangleCalls[0].color);

    resetCanvas();
    mockMillis = 1192;  // exactly two 96 ms intervals after setUp's 1000 ms epoch
    display.ut_drawDirectionArrow(DIR_FRONT, false, 0x20, 0);

    TEST_ASSERT_EQUAL_UINT_MESSAGE(0u, canvas()->fillTriangleCalls.size(),
        "two missed V1 blink intervals should land on the same image phase, not invert it");
}

void test_drawArrow_single_visibility_change_redraws_full_cluster() {
    display.ut_drawDirectionArrow(DIR_ALL, false, 0, 0);  // prime cache with all arrows visible
    resetCanvas();

    display.ut_drawDirectionArrow(static_cast<Direction>(DIR_FRONT | DIR_REAR), false, 0, 0);

    TEST_ASSERT_EQUAL_UINT_MESSAGE(4u, canvas()->fillTriangleCalls.size(),
                                   "single-arrow visibility change must redraw the full cluster");

    size_t biggestIdx = 0;
    uint32_t biggestArea = 0;
    for (size_t i = 0; i < canvas()->fillRectCalls.size(); ++i) {
        uint32_t area = static_cast<uint32_t>(canvas()->fillRectCalls[i].w) *
                        static_cast<uint32_t>(canvas()->fillRectCalls[i].h);
        if (area > biggestArea) {
            biggestArea = area;
            biggestIdx = i;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(!canvas()->fillRectCalls.empty(),
                             "cluster redraw must clear the arrow region");
    TEST_ASSERT_TRUE_MESSAGE(canvas()->fillRectCalls[biggestIdx].h > 60,
                             "cluster redraw must clear beyond a single-arrow band");
}

// ============================================================================
// arrowBoundingRect — static helper used by the live-update partial-flush
// dispatch. The rect must cover every pixel drawDirectionArrow can touch; if
// a future geometry change makes the bounds under-cover the cluster, the
// arrow-only flushRegion call would leave stale pixels on the panel. These
// tests pin the rect against the logical framebuffer for both layouts.
// See docs/plans/ARROW_PARTIAL_FLUSH_20260422.md.
// ============================================================================

void test_arrow_bounding_rect_fits_inside_framebuffer_raised_layout() {
    auto r = V1Display::arrowBoundingRect(/*raisedLayout=*/true);
    TEST_ASSERT_TRUE_MESSAGE(r.w > 0, "raised layout: width must be positive");
    TEST_ASSERT_TRUE_MESSAGE(r.h > 0, "raised layout: height must be positive");
    TEST_ASSERT_TRUE_MESSAGE(r.x >= 0, "raised layout: x within canvas");
    TEST_ASSERT_TRUE_MESSAGE(r.y >= 0, "raised layout: y within canvas");
    TEST_ASSERT_TRUE_MESSAGE(r.x + r.w <= SCREEN_WIDTH,  "raised layout: right edge within canvas");
    TEST_ASSERT_TRUE_MESSAGE(r.y + r.h <= SCREEN_HEIGHT, "raised layout: bottom edge within canvas");
}

void test_arrow_bounding_rect_fits_inside_framebuffer_centered_layout() {
    auto r = V1Display::arrowBoundingRect(/*raisedLayout=*/false);
    TEST_ASSERT_TRUE_MESSAGE(r.w > 0, "centered layout: width must be positive");
    TEST_ASSERT_TRUE_MESSAGE(r.h > 0, "centered layout: height must be positive");
    TEST_ASSERT_TRUE_MESSAGE(r.x >= 0, "centered layout: x within canvas");
    TEST_ASSERT_TRUE_MESSAGE(r.y >= 0, "centered layout: y within canvas");
    TEST_ASSERT_TRUE_MESSAGE(r.x + r.w <= SCREEN_WIDTH,  "centered layout: right edge within canvas");
    TEST_ASSERT_TRUE_MESSAGE(r.y + r.h <= SCREEN_HEIGHT, "centered layout: bottom edge within canvas");
}

void test_arrow_bounding_rect_covers_drawArrow_fillrect_region() {
    // Load-bearing invariant: the bounding rect must cover every pixel the
    // full-redraw branch of drawDirectionArrow clears. If a future geometry
    // change breaks this, arrow-only flushRegion would leave stale pixels.
    //
    // This runs with the default dirty_.multiAlert=false (centered layout);
    // that's the layout the bounds test must match.
    display.ut_elementCaches().arrow.valid = false;  // force full-cluster clear
    resetCanvas();
    display.ut_drawDirectionArrow(DIR_ALL, false, 0, 0);

    // The cluster-clear fillRect is the largest by area.
    auto& fr = canvas()->fillRectCalls;
    TEST_ASSERT_TRUE_MESSAGE(!fr.empty(), "full redraw must issue at least one fillRect");

    size_t biggestIdx = 0;
    uint32_t biggestArea = 0;
    for (size_t i = 0; i < fr.size(); ++i) {
        uint32_t a = static_cast<uint32_t>(fr[i].w) * static_cast<uint32_t>(fr[i].h);
        if (a > biggestArea) { biggestArea = a; biggestIdx = i; }
    }
    const auto& clear = fr[biggestIdx];

    auto r = V1Display::arrowBoundingRect(/*raisedLayout=*/false);
    TEST_ASSERT_TRUE_MESSAGE(r.x <= clear.x,
        "bounding rect must cover left edge of drawArrow clear");
    TEST_ASSERT_TRUE_MESSAGE(r.y <= clear.y,
        "bounding rect must cover top edge of drawArrow clear");
    TEST_ASSERT_TRUE_MESSAGE(r.x + r.w >= clear.x + clear.w,
        "bounding rect must cover right edge of drawArrow clear");
    TEST_ASSERT_TRUE_MESSAGE(r.y + r.h >= clear.y + clear.h,
        "bounding rect must cover bottom edge of drawArrow clear");
}

// ============================================================================
// Test runner
// ============================================================================

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_drawArrow_full_redraw_produces_4_fill_triangles);
    RUN_TEST(test_drawArrow_full_redraw_clears_region_with_bg_color);
    RUN_TEST(test_drawArrow_cache_hit_skips_redraw);
    RUN_TEST(test_drawArrow_dirty_arrow_flag_forces_redraw);
    RUN_TEST(test_drawArrow_direction_change_invalidates_cache);
    RUN_TEST(test_drawArrow_muted_change_invalidates_cache);
    RUN_TEST(test_drawArrow_front_only_top_triangle_uses_front_color);
    RUN_TEST(test_drawArrow_rear_only_bottom_triangle_uses_rear_color);
    RUN_TEST(test_drawArrow_side_only_both_side_triangles_use_side_color);
    RUN_TEST(test_drawArrow_muted_all_triangles_use_muted_color);
    RUN_TEST(test_drawArrow_no_arrows_all_triangles_use_off_color);
    RUN_TEST(test_drawArrow_flash_bit_waits_full_96ms_before_toggling);
    RUN_TEST(test_drawArrow_late_by_two_intervals_keeps_v1_phase);
    RUN_TEST(test_drawArrow_single_visibility_change_redraws_full_cluster);
    RUN_TEST(test_arrow_bounding_rect_fits_inside_framebuffer_raised_layout);
    RUN_TEST(test_arrow_bounding_rect_fits_inside_framebuffer_centered_layout);
    RUN_TEST(test_arrow_bounding_rect_covers_drawArrow_fillrect_region);
    return UNITY_END();
}
