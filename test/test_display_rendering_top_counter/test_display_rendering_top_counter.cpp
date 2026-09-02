/**
 * test_display_rendering_top_counter.cpp
 *
 * Integration tests for display_top_counter.cpp.  These pin the bogey counter
 * to a fixed physical LED cell while preserving the Segment7 OFR font path.
 * The suite verifies that later live font-measurement shifts cannot move a glyph
 * after its placement bounds have been cached.
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
SettingsManager settings;

V1Display::V1Display(SettingsManager& injectedSettings) : settings_(injectedSettings) {
    currentPalette_ = ColorThemes::STANDARD();
    currentPalette_.colorMuted = settings_.get().colorMuted;
    currentPalette_.colorPersisted = settings_.get().colorPersisted;
    g_displayInstance = this;
}
V1Display::~V1Display() = default;

V1Display display(settings);

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

// Reset observations between frames without invalidating the cache under test.
static void resetCounterObservations() {
    canvas()->resetCounters();
    resetFontRecorder();
    display.ut_resetDrawnRegion();
}

static void assertCounterRepaint(bool dot, bool ofr) {
    TEST_ASSERT_EQUAL_UINT(1u, canvas()->fillRectCalls.size());
    const auto& clear = canvas()->fillRectCalls[0];
    TEST_ASSERT_EQUAL_INT16(DisplayLayout::TOP_COUNTER_FIELD_X - 1, clear.x);
    TEST_ASSERT_EQUAL_INT16(77 - clear.x, clear.w);
    TEST_ASSERT_EQUAL_INT16(DisplayLayout::TOP_COUNTER_FIELD_Y, clear.y);
    TEST_ASSERT_EQUAL_INT16(DisplayLayout::TOP_COUNTER_FIELD_H, clear.h);
    TEST_ASSERT_FALSE(display.ut_drawnRegionEmpty());
    TEST_ASSERT_EQUAL_INT16(clear.x, display.ut_drawnRegionX());
    TEST_ASSERT_EQUAL_INT16(clear.w, display.ut_drawnRegionW());
    TEST_ASSERT_EQUAL_INT16(DisplayLayout::kTopCounterRect.y, display.ut_drawnRegionY());
    TEST_ASSERT_EQUAL_INT16(DisplayLayout::kTopCounterRect.h, display.ut_drawnRegionH());
    TEST_ASSERT_EQUAL_UINT(dot ? 1u : 0u, canvas()->fillCircleCalls.size());
    if (dot) {
        const auto& circle = canvas()->fillCircleCalls[0];
        TEST_ASSERT_EQUAL_INT16(4, circle.r);
        TEST_ASSERT_EQUAL_INT16(DisplayLayout::TOP_COUNTER_FIELD_X + DisplayLayout::TOP_COUNTER_FIELD_W -
                               DisplayLayout::TOP_COUNTER_PAD_RIGHT - 5, circle.x);
        TEST_ASSERT_EQUAL_INT16(DisplayLayout::TOP_COUNTER_TEXT_Y + DisplayLayout::TOP_COUNTER_FONT_SIZE - 5,
                               circle.y);
    }
    if (ofr) {
        TEST_ASSERT_EQUAL_INT(1, display.ut_fontMgr().segment7.printfCount);
        TEST_ASSERT_EQUAL_STRING("3", display.ut_fontMgr().segment7.lastPrinted);
    } else {
        TEST_ASSERT_EQUAL_INT(0, display.ut_fontMgr().segment7.printfCount);
        TEST_ASSERT_GREATER_THAN_UINT(0u, canvas()->fillRoundRectCalls.size());
    }
    TEST_ASSERT_EQUAL_INT(0, canvas()->getFlushCount());
}

static void assertCounterNoPaint() {
    TEST_ASSERT_EQUAL_UINT(0u, canvas()->fillRectCalls.size());
    TEST_ASSERT_EQUAL_UINT(0u, canvas()->fillRoundRectCalls.size());
    TEST_ASSERT_EQUAL_UINT(0u, canvas()->fillCircleCalls.size());
    TEST_ASSERT_EQUAL_INT(0, display.ut_fontMgr().segment7.printfCount);
    TEST_ASSERT_TRUE(display.ut_drawnRegionEmpty());
    TEST_ASSERT_EQUAL_INT(0, canvas()->getFlushCount());
}

static void assertSameDigitDotTransition(bool initialDot, bool ofr) {
    display.ut_fontMgr().segment7Ready = ofr;
    resetCounterObservations();
    display.ut_drawTopCounterPair('3', false, initialDot);
    assertCounterRepaint(initialDot, ofr);

    resetCounterObservations();
    display.ut_drawTopCounterPair('3', false, !initialDot);
    assertCounterRepaint(!initialDot, ofr);

    resetCounterObservations();
    display.ut_drawTopCounterPair('3', false, !initialDot);
    assertCounterNoPaint();

    display.ut_elementCaches().topCounter.invalidate();
    display.ut_drawTopCounterPair('3', false, !initialDot);
    assertCounterRepaint(!initialDot, ofr);
}

void test_same_digit_dot_on_repaints_with_ofr() {
    assertSameDigitDotTransition(false, true);
}
void test_same_digit_dot_off_clears_previously_painted_dot_with_ofr() {
    assertSameDigitDotTransition(true, true);
}
void test_same_digit_dot_on_repaints_with_fallback() {
    assertSameDigitDotTransition(false, false);
}
void test_same_digit_dot_off_clears_previously_painted_dot_with_fallback() {
    assertSameDigitDotTransition(true, false);
}

void test_fixed_dot_preserves_mute_and_color_cache_keys() {
    display.ut_drawTopCounterPair('3', false, true);
    resetCounterObservations();
    display.ut_drawTopCounterPair('3', true, true);
    assertCounterRepaint(true, true);
    // Numeric glyphs/dots retain the configured bogey color even when muted.
    TEST_ASSERT_EQUAL_UINT16(settings.get().colorBogey, canvas()->fillCircleCalls[0].color);

    const uint16_t originalColor = settings.get().colorBogey;
    settings.mutableSettings().colorBogey ^= 1;
    resetCounterObservations();
    display.ut_drawTopCounterPair('3', true, true);
    assertCounterRepaint(true, true);
    TEST_ASSERT_EQUAL_UINT16(settings.get().colorBogey, canvas()->fillCircleCalls[0].color);
    settings.mutableSettings().colorBogey = originalColor;
}

void test_text_and_paired_dots_keep_their_existing_layout_and_cache_semantics() {
    display.ut_drawTopCounterPair('P', false, true);
    TEST_ASSERT_EQUAL_STRING("P.", display.ut_fontMgr().segment7.lastPrinted);
    TEST_ASSERT_EQUAL_UINT(0u, canvas()->fillCircleCalls.size());
    resetCounterObservations();
    display.ut_drawTopCounterPair('\0', false, true);
    TEST_ASSERT_EQUAL_STRING(" .", display.ut_fontMgr().segment7.lastPrinted);
    TEST_ASSERT_EQUAL_UINT(0u, canvas()->fillCircleCalls.size());
    resetCounterObservations();
    display.ut_drawTopCounterPair(' ', false, true);
    assertCounterNoPaint(); // Null and space normalize to the same text.

    display.ut_drawTopCounterPair('3', false, true, '4', true);
    TEST_ASSERT_EQUAL_STRING("3.4.", display.ut_fontMgr().segment7.lastPrinted);
    TEST_ASSERT_EQUAL_UINT(0u, canvas()->fillCircleCalls.size());
    resetCounterObservations();
    display.ut_drawTopCounterPair('3', false, true, '4', true);
    assertCounterNoPaint();
    display.ut_drawTopCounterPair('3', false, true);
    assertCounterRepaint(true, true);
    resetCounterObservations();
    display.ut_drawTopCounterPair('3', false, true);
    assertCounterNoPaint();
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_top_counter_uses_ofr_segment7_when_ready);
    RUN_TEST(test_top_counter_transition_to_narrow_digit_keeps_same_clear_window);
    RUN_TEST(test_top_counter_narrow_digit_keeps_same_ofr_cursor_as_full_digit);
    RUN_TEST(test_top_counter_mode_and_volume_symbols_share_locked_cell);
    RUN_TEST(test_top_counter_cursor_ignores_corrupted_live_measure);
    RUN_TEST(test_top_counter_fixed_dot_stays_inside_field);
    RUN_TEST(test_same_digit_dot_on_repaints_with_ofr);
    RUN_TEST(test_same_digit_dot_off_clears_previously_painted_dot_with_ofr);
    RUN_TEST(test_same_digit_dot_on_repaints_with_fallback);
    RUN_TEST(test_same_digit_dot_off_clears_previously_painted_dot_with_fallback);
    RUN_TEST(test_fixed_dot_preserves_mute_and_color_cache_keys);
    RUN_TEST(test_text_and_paired_dots_keep_their_existing_layout_and_cache_semantics);
    return UNITY_END();
}
