/**
 * Integration coverage for display_frequency.cpp render-cache dispatch.
 */

#include <unity.h>

#include "../mocks/display_driver.h"
#include "../mocks/Arduino.h"
#include "../mocks/settings.h"
#include "../mocks/packet_parser.h"
#include "../mocks/esp_heap_caps.h"
#include "../mocks/mock_heap_caps_state.h"

#ifndef ARDUINO
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
SerialClass Serial;
#endif

#include "../../src/display.h"
#include "../../src/perf_metrics.h"
#include "../../include/display_element_caches.h"

V1Display* g_displayInstance = nullptr;
SettingsManager settingsManager;

static int g_frequencyRedrawReasons = 0;
void perfRecordDisplayRedrawReason(PerfDisplayRedrawReason reason) {
    if (reason == PerfDisplayRedrawReason::FrequencyChange) {
        ++g_frequencyRedrawReasons;
    }
}

V1Display::V1Display() {
    currentPalette_ = ColorThemes::STANDARD();
    currentPalette_.colorMuted = settingsManager.get().colorMuted;
    currentPalette_.colorPersisted = settingsManager.get().colorPersisted;
    g_displayInstance = this;
}
V1Display::~V1Display() = default;

uint16_t V1Display::getBandColor(Band band) {
    const V1Settings& s = settingsManager.get();
    switch (band) {
        case BAND_X: return s.colorBandX;
        case BAND_K: return s.colorBandK;
        case BAND_KA: return s.colorBandKa;
        case BAND_LASER: return s.colorBandL;
        default: return s.colorFrequency;
    }
}

int V1Display::measureSevenSegmentText(const char* text, float) const {
    return text ? static_cast<int>(strlen(text) * 8) : 0;
}

int V1Display::drawSevenSegmentText(const char*, int, int, float, uint16_t, uint16_t) {
    return 0;
}

int V1Display::draw14SegmentText(const char*, int, int, float, uint16_t, uint16_t) {
    return 0;
}

#include "../../src/display_frequency_digit_atlas.cpp"
#include "../../src/display_frequency_raster_cache.cpp"
#include "../../src/display_frequency.cpp"

V1Display display;

namespace {

Arduino_Canvas* canvas() { return display.testCanvas(); }

void fillFramebuffer(uint16_t color) {
    uint16_t* framebuffer = canvas()->getFramebuffer();
    for (size_t i = 0; i < static_cast<size_t>(CANVAS_WIDTH) * CANVAS_HEIGHT; ++i) {
        framebuffer[i] = color;
    }
}

void storeFrequencyAtlasCells() {
    DisplayFrequencyDigitAtlas& atlas = display.ut_frequencyDigitAtlas();
    TEST_ASSERT_TRUE(atlas.begin(6, 8));

    fillFramebuffer(0xFFFF);
    uint16_t* framebuffer = canvas()->getFramebuffer();
    constexpr uint16_t kBg = 0x0000;
    for (uint8_t pos = 0; pos < DisplayFrequencyDigitAtlas::kTextPositions; ++pos) {
        const int16_t x = static_cast<int16_t>(150 + pos * 10);
        if (pos == 2) {
            TEST_ASSERT_TRUE(atlas.storeCell(pos, '.', x, 40, 6, 8, framebuffer, CANVAS_WIDTH, kBg));
            continue;
        }
        for (char digit = '0'; digit <= '9'; ++digit) {
            TEST_ASSERT_TRUE(atlas.storeCell(pos, digit, x, 40, 6, 8, framebuffer, CANVAS_WIDTH, kBg));
        }
    }
    TEST_ASSERT_TRUE(atlas.ready());
    fillFramebuffer(kBg);
}

void prepareDisplay() {
    mock_reset_heap_caps();
    mockMillis = 1000;
    mockMicros = 1000;
    g_frequencyRedrawReasons = 0;
    display.setTestCanvas(new Arduino_Canvas(SCREEN_WIDTH, SCREEN_HEIGHT, nullptr));
    canvas()->resetCounters();
    display.ut_elementCaches() = DisplayElementCaches{};
    display.ut_fontMgr().segment7Ready = true;
    display.ut_fontMgr().segment7.resetRecordedCalls();
    display.ut_resetDrawnRegion();
    storeFrequencyAtlasCells();
}

}  // namespace

void setUp() {
    prepareDisplay();
}

void tearDown() {}

void test_numeric_frequency_delta_clears_only_changed_digit_cell() {
    display.ut_drawFrequency(34123, BAND_KA, false, false);  // 34.123
    TEST_ASSERT_EQUAL_UINT(1u, canvas()->fillRectCalls.size());
    TEST_ASSERT_GREATER_THAN_INT(100, canvas()->fillRectCalls[0].w);

    canvas()->resetCounters();
    display.ut_resetDrawnRegion();

    display.ut_drawFrequency(34133, BAND_KA, false, false);  // 34.133

    TEST_ASSERT_EQUAL_UINT(1u, canvas()->fillRectCalls.size());
    const auto& call = canvas()->fillRectCalls[0];
    TEST_ASSERT_EQUAL_INT16(190, call.x);  // position 4 cell from storeFrequencyAtlasCells()
    TEST_ASSERT_EQUAL_INT16(40, call.y);
    TEST_ASSERT_EQUAL_INT16(6, call.w);
    TEST_ASSERT_EQUAL_INT16(8, call.h);
    TEST_ASSERT_EQUAL_INT16(190, display.ut_drawnRegionX());
    TEST_ASSERT_EQUAL_INT16(40, display.ut_drawnRegionY());
    TEST_ASSERT_EQUAL_INT16(6, display.ut_drawnRegionW());
    TEST_ASSERT_EQUAL_INT16(8, display.ut_drawnRegionH());
    TEST_ASSERT_EQUAL_UINT32(2u, display.ut_frequencyDigitAtlas().hitCount());
}

void test_numeric_frequency_color_change_uses_full_frequency_clear() {
    display.ut_drawFrequency(34123, BAND_KA, false, false);
    canvas()->resetCounters();
    display.ut_resetDrawnRegion();

    display.ut_drawFrequency(34123, BAND_KA, true, false);

    TEST_ASSERT_EQUAL_UINT(1u, canvas()->fillRectCalls.size());
    const auto& call = canvas()->fillRectCalls[0];
    TEST_ASSERT_GREATER_THAN_INT(100, call.w);
    TEST_ASSERT_GREATER_THAN_INT(50, call.h);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_numeric_frequency_delta_clears_only_changed_digit_cell);
    RUN_TEST(test_numeric_frequency_color_change_uses_full_frequency_clear);
    return UNITY_END();
}
