/**
 * test_display_rendering_top_counter.cpp
 *
 * Integration tests for display_top_counter.cpp.  These pin the bogey counter
 * to a fixed physical LED cell while preserving the Segment7 OFR font path.
 */

#include <unity.h>
#include <cstdint>
#include <cstring>

#include "../mocks/display_driver.h"
#include "../mocks/Arduino.h"
#include "../mocks/settings.h"
#include "../mocks/packet_parser.h"

#ifndef ARDUINO
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
SerialClass Serial;
#endif

#include "../../src/display.h"
#include "../../include/display_dirty_flags.h"
#include "../../include/display_element_caches.h"
#include "../../include/display_layout.h"

V1Display* g_displayInstance = nullptr;
SettingsManager settingsManager;

V1Display::V1Display() {
    currentPalette_ = ColorThemes::STANDARD();
    currentPalette_.colorMuted = settingsManager.get().colorMuted;
    currentPalette_.colorPersisted = settingsManager.get().colorPersisted;
    g_displayInstance = this;
}
V1Display::~V1Display() = default;

V1Display display;

#include "../../src/perf_metrics.h"
void perfRecordDisplayRedrawReason(PerfDisplayRedrawReason) {}

bool DisplayFontManager::getTopCounterBounds(char symbol, bool showDot, int& xMin, int& xMax) {
    // Mirror production semantics: bounds come from a boot-primed cache and
    // never change afterwards. First lookup measures (boot prime); later
    // lookups replay the cached value even if the live measure has since
    // been corrupted (see mock OpenFontRender::bboxShiftX).
    static int16_t cachedMin[128][2];
    static int16_t cachedMax[128][2];
    static bool cached[128][2];
    const uint8_t c = static_cast<uint8_t>(symbol) & 0x7F;
    const uint8_t d = showDot ? 1 : 0;
    if (!cached[c][d]) {
        char text[3] = {symbol, 0, 0};
        if (showDot) {
            text[1] = '.';
        }
        FT_BBox bbox = segment7.calculateBoundingBox(
            0, 0, DisplayLayout::TOP_COUNTER_FONT_SIZE, Align::Left, Layout::Horizontal, text);
        cachedMin[c][d] = static_cast<int16_t>(bbox.xMin);
        cachedMax[c][d] = static_cast<int16_t>(bbox.xMax);
        cached[c][d] = true;
    }
    xMin = cachedMin[c][d];
    xMax = cachedMax[c][d];
    return true;
}

#include "../../src/display_top_counter.cpp"

static Arduino_Canvas* canvas() { return display.testCanvas(); }

static void resetCanvas() {
    display.setTestCanvas(new Arduino_Canvas(SCREEN_WIDTH, SCREEN_HEIGHT, nullptr));
    canvas()->resetCounters();
}

static void resetFontRecorder() {
    display.ut_fontMgr().segment7.resetRecordedCalls();
}

static int16_t cursorAfterDrawing(char symbol, bool dot = false) {
    resetCanvas();
    resetFontRecorder();
    display.ut_elementCaches().topCounter.counterValid = false;
    display.ut_drawTopCounterPair(symbol, false, dot);
    TEST_ASSERT_EQUAL_INT(1, display.ut_fontMgr().segment7.printfCount);
    TEST_ASSERT_EQUAL_CHAR(symbol, display.ut_fontMgr().segment7.lastPrinted[0]);
    return display.ut_fontMgr().segment7.lastCursorX;
}

void setUp() {
    mockMillis = 1000;
    resetCanvas();
    display.ut_elementCaches() = DisplayElementCaches{};
    display.ut_fontMgr().segment7Ready = true;
    resetFontRecorder();
}

void tearDown() {}

void test_top_counter_uses_ofr_segment7_when_ready() {
    display.ut_drawTopCounterPair('8', false, false);

    TEST_ASSERT_EQUAL_UINT(0u, canvas()->fillRoundRectCalls.size());
    TEST_ASSERT_EQUAL_INT(1, display.ut_fontMgr().segment7.printfCount);
    TEST_ASSERT_EQUAL_STRING("8", display.ut_fontMgr().segment7.lastPrinted);
}

void test_top_counter_transition_to_narrow_digit_keeps_same_clear_window() {
    display.ut_drawTopCounterPair('8', false, false);
    resetCanvas();
    resetFontRecorder();

    display.ut_drawTopCounterPair('1', false, false);

    TEST_ASSERT_EQUAL_UINT(1u, canvas()->fillRectCalls.size());
    TEST_ASSERT_EQUAL_INT16(DisplayLayout::TOP_COUNTER_FIELD_X - 1,
                            canvas()->fillRectCalls[0].x);
    TEST_ASSERT_EQUAL_INT16(77 - (DisplayLayout::TOP_COUNTER_FIELD_X - 1),
                            canvas()->fillRectCalls[0].w);
    TEST_ASSERT_EQUAL_INT(1, display.ut_fontMgr().segment7.printfCount);
    TEST_ASSERT_EQUAL_STRING("1", display.ut_fontMgr().segment7.lastPrinted);
}

void test_top_counter_narrow_digit_keeps_same_ofr_cursor_as_full_digit() {
    const int16_t cursor8 = cursorAfterDrawing('8');
    const int16_t cursor1 = cursorAfterDrawing('1');

    TEST_ASSERT_EQUAL_INT16(cursor8, cursor1);
}

void test_top_counter_mode_and_volume_symbols_share_locked_cell() {
    const int16_t referenceCursor = cursorAfterDrawing('8');
    static constexpr char kSymbols[] = {'A', 'L', 'c', 'u', 'P', 'J', '5'};
    for (char symbol : kSymbols) {
        TEST_ASSERT_EQUAL_INT16(referenceCursor, cursorAfterDrawing(symbol));
    }
}

void test_top_counter_cursor_ignores_corrupted_live_measure() {
    // Regression: bench runs 8a599c91/1533471d rendered the same '1' 22 px
    // apart because OFR's live calculateBoundingBox returned a corrupted
    // xMin under FreeType cache pressure and the cursor clamp consumed it.
    // Single-glyph layout must take bounds from the boot-primed cache, so a
    // later live-measure corruption cannot move the digit.
    const int16_t cleanCursor = cursorAfterDrawing('1');  // primes the stub cache

    display.ut_fontMgr().segment7.bboxShiftX = -40;
    const int16_t poisonedCursor = cursorAfterDrawing('1');
    display.ut_fontMgr().segment7.bboxShiftX = 0;

    TEST_ASSERT_EQUAL_INT16(cleanCursor, poisonedCursor);
}

void test_top_counter_fixed_dot_stays_inside_field() {
    display.ut_drawTopCounterPair('0', false, true);

    TEST_ASSERT_GREATER_THAN_UINT(0u, canvas()->fillCircleCalls.size());
    const auto& dot = canvas()->fillCircleCalls.back();
    const int16_t fieldRight = static_cast<int16_t>(DisplayLayout::TOP_COUNTER_FIELD_X +
                                                   DisplayLayout::TOP_COUNTER_FIELD_W);
    TEST_ASSERT_LESS_OR_EQUAL_INT16(fieldRight, static_cast<int16_t>(dot.x + dot.r));
    TEST_ASSERT_EQUAL_INT(1, display.ut_fontMgr().segment7.printfCount);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_top_counter_uses_ofr_segment7_when_ready);
    RUN_TEST(test_top_counter_transition_to_narrow_digit_keeps_same_clear_window);
    RUN_TEST(test_top_counter_narrow_digit_keeps_same_ofr_cursor_as_full_digit);
    RUN_TEST(test_top_counter_mode_and_volume_symbols_share_locked_cell);
    RUN_TEST(test_top_counter_cursor_ignores_corrupted_live_measure);
    RUN_TEST(test_top_counter_fixed_dot_stays_inside_field);
    return UNITY_END();
}
