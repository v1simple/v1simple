/**
 * test_display_rendering_indicators.cpp
 *
 * Phase 3 Task 3.4 — integration tests for display_indicators.cpp
 * (drawObdIndicator, drawBaseFrame).
 *
 * Includes the real rendering source so that GFX call-recording assertions
 * on the injected Arduino_Canvas verify actual draw behaviour.
 *
 * NOTE: Drawing logic in display_indicators.cpp is guarded by
 * #if defined(DISPLAY_WAVESHARE_349) — we define it here to enable it.
 */

// Enable the display-variant guards before any display headers are pulled in.
#ifndef DISPLAY_WAVESHARE_349
#define DISPLAY_WAVESHARE_349 1
#endif

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
#include "../../src/perf_metrics.h"
#include "../../include/display_dirty_flags.h"
#include "../../include/display_element_caches.h"

// ---------------------------------------------------------------------------
// Required extern definitions
// ---------------------------------------------------------------------------
V1Display* g_displayInstance = nullptr;
SettingsManager settingsManager;

void perfRecordDisplayStatusPaint(PerfDisplayStatusPaint) {}

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

// Global test display instance
V1Display display;

// ---------------------------------------------------------------------------
// Real rendering code under test.
// display_indicators.cpp includes the OBD module headers from src/ — those
// compile fine on native because FreeRTOS is mocked in test/mocks/freertos/.
// ---------------------------------------------------------------------------
#include "../../src/display_indicators.cpp"

// ---------------------------------------------------------------------------
// Stub for drawBLEProxyIndicator (defined in display_status_bar.cpp which is
// not compiled here; prepareFullRedrawNoClear() calls it after a full clear).
// ---------------------------------------------------------------------------
void V1Display::drawBLEProxyIndicator() {}

// ---------------------------------------------------------------------------
// Module globals required by display_indicators.cpp externs.
// snapshot() is never called in these tests so the implementations are not
// needed; only the type definition from the headers is required.
// ---------------------------------------------------------------------------
ObdRuntimeModule obdRuntimeModule;
AlpRuntimeModule alpRuntimeModule;
GpsRuntimeModule gpsRuntimeModule;

// Stub snapshot() implementations so the linker satisfies syncTopIndicators()
// references even though syncTopIndicators() is never called in these tests.
ObdRuntimeStatus ObdRuntimeModule::snapshot(uint32_t /*nowMs*/) const {
    return ObdRuntimeStatus{};
}
AlpStatus AlpRuntimeModule::snapshot() const {
    return AlpStatus{};
}
GpsRuntimeStatus GpsRuntimeModule::snapshot(uint32_t /*nowMs*/) const {
    return GpsRuntimeStatus{};
}

const char* alpGunAbbrev(AlpGunType gun) {
    switch (gun) {
        case AlpGunType::MARKSMAN_ULTRALYTE: return "ULT";
        case AlpGunType::PL3_PROLITE: return "PL3";
        case AlpGunType::DRAGONEYE_COMPACT: return "DE";
        case AlpGunType::LASER_ATLANTA_PL2: return "PL2";
        case AlpGunType::LTI_TRUSPEED_LR: return "TSLR";
        default: return "LASER";
    }
}

void AlpRuntimeModule::logDisplayDecision(uint32_t, const char*, const char*) {}

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------
static Arduino_Canvas* canvas() { return display.testCanvas(); }

static void resetCanvas() {
    display.setTestCanvas(new Arduino_Canvas(SCREEN_WIDTH, SCREEN_HEIGHT, nullptr));
    canvas()->resetCounters();
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
// drawBaseFrame tests
// ============================================================================

void test_drawBaseFrame_fills_screen_with_bg_color() {
    display.ut_drawBaseFrame();
    TEST_ASSERT_EQUAL_INT(1, canvas()->getFillScreenCount());
    TEST_ASSERT_EQUAL_UINT16(ColorThemes::STANDARD().bg, canvas()->getLastFillColor());
}

void test_drawBaseFrame_sets_all_dirty_flags() {
    // After drawBaseFrame, dirty.setIndicatorFlags() runs (formerly setAll) —
    // verify the element caches whose re-render it gates (bands/arrow) are
    // invalidated so the next frame rebuilds them.
    display.ut_drawBaseFrame();
    TEST_ASSERT_FALSE(display.ut_elementCaches().bands.valid);
    TEST_ASSERT_FALSE(display.ut_elementCaches().arrow.valid);
}

// ============================================================================
// drawObdIndicator tests
// ============================================================================

void test_drawObdIndicator_enabled_connected_draws_text() {
    display.ut_setObdStatus(true, true, false);
    display.ut_drawObdIndicator();

    // One fillRect (background clear) + text draw
    TEST_ASSERT_GREATER_OR_EQUAL(1u, canvas()->fillRectCalls.size());
    TEST_ASSERT_EQUAL_UINT16(ColorThemes::STANDARD().bg, canvas()->fillRectCalls[0].color);
}

void test_drawObdIndicator_disabled_clears_area() {
    display.ut_setObdStatus(false, false, false);
    display.ut_drawObdIndicator();

    // Disabled: clears with BG and returns
    TEST_ASSERT_GREATER_OR_EQUAL(1u, canvas()->fillRectCalls.size());
    TEST_ASSERT_EQUAL_UINT16(ColorThemes::STANDARD().bg, canvas()->fillRectCalls[0].color);
}

void test_drawObdIndicator_cache_hit_skips_redraw() {
    display.ut_setObdStatus(true, false, false);
    display.ut_drawObdIndicator();  // primes cache
    resetCanvas();

    display.ut_drawObdIndicator();  // same state, no dirty → cache hit
    TEST_ASSERT_EQUAL_UINT(0u, canvas()->fillRectCalls.size());
}

// ============================================================================
// Test runner
// ============================================================================

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_drawBaseFrame_fills_screen_with_bg_color);
    RUN_TEST(test_drawBaseFrame_sets_all_dirty_flags);
    RUN_TEST(test_drawObdIndicator_enabled_connected_draws_text);
    RUN_TEST(test_drawObdIndicator_disabled_clears_area);
    RUN_TEST(test_drawObdIndicator_cache_hit_skips_redraw);
    return UNITY_END();
}
