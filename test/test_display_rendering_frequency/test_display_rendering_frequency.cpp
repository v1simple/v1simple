/**
 * Call-recording tests for the primary frequency renderer.
 *
 * The radar input is built from the ESP alert-row field layout and decoded by
 * the real PacketParser. Assertions stop at the renderer command boundary;
 * the native font mock does not claim glyph or panel fidelity.
 */

#ifndef DISPLAY_WAVESHARE_349
#define DISPLAY_WAVESHARE_349 1
#endif

#include <unity.h>

#include <cstring>
#include <vector>

// Mocks first so production display headers bind to the recording driver.
#include "../mocks/display_driver.h"
#include "../mocks/Arduino.h"
#include "../mocks/settings.h"

#ifndef ARDUINO
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
SerialClass Serial;
#endif

#include "../../src/display.h"
#include "../../src/packet_parser.h"

V1Display* g_displayInstance = nullptr;
SettingsManager settings;

V1Display::V1Display(SettingsManager& injectedSettings) : settings_(injectedSettings) {
    currentPalette_ = ColorThemes::STANDARD();
    currentPalette_.colorMuted = settings_.get().colorMuted;
    currentPalette_.colorPersisted = settings_.get().colorPersisted;
    g_displayInstance = this;
}
V1Display::~V1Display() = default;

uint16_t V1Display::getBandColor(Band band) {
    const V1Settings& s = settings_.get();
    switch (band) {
        case BAND_LASER: return s.colorBandL;
        case BAND_KA: return s.colorBandKa;
        case BAND_K:
        case BAND_KU: return s.colorBandK;
        case BAND_X: return s.colorBandX;
        default: return currentPalette_.text;
    }
}

#include "../../src/display_frequency.cpp"
#include "../../src/display_top_counter.cpp"
#include "../../src/display_frequency_digit_atlas.cpp"
#include "../../src/display_frequency_raster_cache.cpp"
#include "../../src/packet_parser.cpp"
#include "../../src/packet_parser_alerts.cpp"

// Top-counter font priming is outside this frequency suite.
bool DisplayFontManager::getTopCounterBounds(char, bool, int&, int&) { return false; }

V1Display display(settings);

namespace {

Arduino_Canvas* canvas() {
    return display.testCanvas();
}

OpenFontRender& frequencyFont() {
    return display.ut_fontMgr().segment7;
}

void resetRecordedOutput() {
    canvas()->resetCounters();
    frequencyFont().resetRecordedCalls();
}

AlertData radarAlertFromSpecFields(uint16_t frequencyMHz, uint8_t bandBits, uint8_t directionBits) {
    // ESP Spec alert row: index/count, frequency MSB/LSB, front/rear RSSI,
    // band+direction, aux0. 0xA0 is a valid K-band strength stimulus.
    std::vector<uint8_t> packet = {
        0xAA, 0xD8, 0xEA, 0x43, 8, 0x11,
        static_cast<uint8_t>(frequencyMHz >> 8), static_cast<uint8_t>(frequencyMHz),
        0xA0, 0x00, static_cast<uint8_t>(bandBits | directionBits), 0x80};
    uint8_t checksum = 0;
    for (uint8_t byte : packet) {
        checksum = static_cast<uint8_t>(checksum + byte);
    }
    packet.push_back(checksum);
    packet.push_back(0xAB);

    PacketParser parser;
    TEST_ASSERT_TRUE(parser.parse(packet.data(), packet.size(), 1000));
    TEST_ASSERT_EQUAL_UINT(1, parser.getAlertCount());
    return parser.getPriorityAlert();
}

void assertFrequencyText(const char* expected) {
    TEST_ASSERT_EQUAL_INT(1, frequencyFont().printfCount);
    TEST_ASSERT_EQUAL_STRING(expected, frequencyFont().lastPrinted);
    TEST_ASSERT_GREATER_THAN_UINT_MESSAGE(0u, canvas()->fillRectCalls.size(),
                                          "a changed frequency must clear its previous drawing area");
}

} // namespace

void setUp() {
    display.~V1Display();
    new (&display) V1Display(settings);
    display.setTestCanvas(new Arduino_Canvas(SCREEN_WIDTH, SCREEN_HEIGHT, nullptr));
    display.ut_elementCaches() = DisplayElementCaches{};
    display.ut_fontMgr().segment7Ready = true;
    mockMillis = 1000;
    mockMicros = 1000;
    resetRecordedOutput();
}

void tearDown() {}

void test_parsed_k_frequency_is_handed_to_renderer_in_ghz_text() {
    const AlertData alert = radarAlertFromSpecFields(24150, 0x04, 0x20);
    TEST_ASSERT_EQUAL_INT(BAND_K, alert.band);
    TEST_ASSERT_EQUAL_INT(DIR_FRONT, alert.direction);
    TEST_ASSERT_EQUAL_UINT32(24150, alert.frequency);

    display.ut_drawFrequency(alert.frequency, alert.band);

    assertFrequencyText("24.150");
}

void test_laser_zero_and_alp_presentations_are_distinct() {
    display.ut_drawFrequency(0, BAND_LASER);
    assertFrequencyText("LASER");

    display.ut_elementCaches().frequency.invalidate();
    resetRecordedOutput();
    display.ut_drawFrequency(0, BAND_NONE);
    assertFrequencyText("--.---");

    display.ut_elementCaches().frequency.invalidate();
    resetRecordedOutput();
    display.ut_drawFrequency(0, BAND_LASER, "LTI");
    assertFrequencyText("LTI");
}

void test_changed_frequency_clears_and_draws_the_new_digits() {
    const AlertData first = radarAlertFromSpecFields(24150, 0x04, 0x20);
    const AlertData changed = radarAlertFromSpecFields(24151, 0x04, 0x20);
    display.ut_drawFrequency(first.frequency, first.band);
    resetRecordedOutput();

    display.ut_drawFrequency(changed.frequency, changed.band);

    assertFrequencyText("24.151");
}

void test_unchanged_frequency_produces_no_paint_commands() {
    const AlertData alert = radarAlertFromSpecFields(24150, 0x04, 0x20);
    display.ut_drawFrequency(alert.frequency, alert.band);
    resetRecordedOutput();

    display.ut_drawFrequency(alert.frequency, alert.band);

    TEST_ASSERT_EQUAL_INT(0, frequencyFont().printfCount);
    TEST_ASSERT_EQUAL_UINT(0u, canvas()->fillRectCalls.size());
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_parsed_k_frequency_is_handed_to_renderer_in_ghz_text);
    RUN_TEST(test_laser_zero_and_alp_presentations_are_distinct);
    RUN_TEST(test_changed_frequency_clears_and_draws_the_new_digits);
    RUN_TEST(test_unchanged_frequency_produces_no_paint_commands);
    return UNITY_END();
}
