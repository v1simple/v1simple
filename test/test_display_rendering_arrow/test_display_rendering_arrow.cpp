/**
 * Call-recording tests for the direction-arrow renderer.
 *
 * Direction inputs are decoded from real ESP alert-row fields. Geometry and
 * cache assertions stop at the renderer command boundary; they do not claim
 * physical panel fidelity.
 */

#include <unity.h>

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

// Invoke the existing private renderer without adding a firmware test hook.
#define private public
#include "../../src/display.h"
#undef private
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

void V1Display::drawBatteryIndicator() {}

#include "../../src/display_arrow.cpp"
#include "../../src/packet_parser.cpp"
#include "../../src/packet_parser_alerts.cpp"

V1Display display(settings);

namespace {

constexpr uint8_t kSpecBandK = 0x04;
constexpr uint8_t kSpecDirectionFront = 0x20;
constexpr uint8_t kSpecDirectionSide = 0x40;
constexpr uint8_t kSpecDirectionRear = 0x80;

Arduino_Canvas* canvas() {
    return display.testCanvas();
}

void resetRecordedOutput() {
    canvas()->resetCounters();
}

AlertData radarAlertFromSpecFields(uint8_t directionBits) {
    // ESP Spec alert row: index/count, frequency MSB/LSB, front/rear RSSI,
    // band+direction, aux0. This represents a priority K alert at 24.150 GHz.
    std::vector<uint8_t> packet = {
        0xAA, 0xD8, 0xEA, 0x43, 8, 0x11, 0x5E, 0x56,
        0xA0, 0x00, static_cast<uint8_t>(kSpecBandK | directionBits), 0x80};
    uint8_t checksum = 0;
    for (uint8_t byte : packet) {
        checksum = static_cast<uint8_t>(checksum + byte);
    }
    packet.push_back(checksum);
    packet.push_back(0xAB);

    PacketParser parser;
    TEST_ASSERT_TRUE(parser.parse(packet.data(), packet.size(), 1000));
    TEST_ASSERT_EQUAL_UINT(1, parser.getAlertCount());
    const AlertData alert = parser.getPriorityAlert();
    TEST_ASSERT_EQUAL_INT(BAND_K, alert.band);
    TEST_ASSERT_EQUAL_UINT32(24150, alert.frequency);
    return alert;
}

Direction directionBitmapFromDisplayBits(uint8_t directionBits) {
    // Unlike one alert-table row, the InfDisplayData image contains three
    // independent direction LEDs and can legitimately report them together.
    return static_cast<Direction>((directionBits >> 5) & 0x07);
}

void drawRadarDirection(const AlertData& alert) {
    display.drawDirectionArrow(alert.direction, false, 0);
}

void assertTriangle(int index, int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                    int16_t x2, int16_t y2, uint16_t color) {
    TEST_ASSERT_GREATER_THAN_INT(index, static_cast<int>(canvas()->fillTriangleCalls.size()));
    const auto& call = canvas()->fillTriangleCalls[static_cast<size_t>(index)];
    TEST_ASSERT_EQUAL_INT16(x0, call.x0);
    TEST_ASSERT_EQUAL_INT16(y0, call.y0);
    TEST_ASSERT_EQUAL_INT16(x1, call.x1);
    TEST_ASSERT_EQUAL_INT16(y1, call.y1);
    TEST_ASSERT_EQUAL_INT16(x2, call.x2);
    TEST_ASSERT_EQUAL_INT16(y2, call.y2);
    TEST_ASSERT_EQUAL_HEX16(color, call.color);
}

} // namespace

void setUp() {
    settings.mutableSettings() = V1Settings{};
    display.~V1Display();
    new (&display) V1Display(settings);
    display.setTestCanvas(new Arduino_Canvas(SCREEN_WIDTH, SCREEN_HEIGHT, nullptr));
    display.elementCaches_ = DisplayElementCaches{};
    display.dirty_.multiAlert = true;
    display.blinkPhase_ = true;
    display.lastBlinkToggleMs_ = 1000;
    mockMillis = 1000;
    mockMicros = 1000;
    resetRecordedOutput();
}

void tearDown() {}

void test_front_spec_direction_draws_the_apex_up_glyph() {
    const AlertData alert = radarAlertFromSpecFields(kSpecDirectionFront);
    TEST_ASSERT_EQUAL_INT(DIR_FRONT, alert.direction);

    drawRadarDirection(alert);

    TEST_ASSERT_EQUAL_UINT(4, canvas()->fillTriangleCalls.size());
    assertTriangle(0, 564, 2, 503, 70, 625, 70, settings.get().colorArrowFront);
    assertTriangle(3, 564, 146, 503, 118, 625, 118, TFT_DARKGREY);
}

void test_rear_spec_direction_draws_the_apex_down_glyph() {
    const AlertData alert = radarAlertFromSpecFields(kSpecDirectionRear);
    TEST_ASSERT_EQUAL_INT(DIR_REAR, alert.direction);

    drawRadarDirection(alert);

    TEST_ASSERT_EQUAL_UINT(4, canvas()->fillTriangleCalls.size());
    assertTriangle(0, 564, 2, 503, 70, 625, 70, TFT_DARKGREY);
    assertTriangle(3, 564, 146, 503, 118, 625, 118, settings.get().colorArrowRear);
}

void test_side_spec_direction_activates_the_bar_and_side_heads_only() {
    const AlertData alert = radarAlertFromSpecFields(kSpecDirectionSide);
    TEST_ASSERT_EQUAL_INT(DIR_SIDE, alert.direction);

    drawRadarDirection(alert);

    TEST_ASSERT_EQUAL_UINT(4, canvas()->fillRectCalls.size());
    const auto& bar = canvas()->fillRectCalls[2];
    TEST_ASSERT_EQUAL_INT16(532, bar.x);
    TEST_ASSERT_EQUAL_INT16(84, bar.y);
    TEST_ASSERT_EQUAL_INT16(64, bar.w);
    TEST_ASSERT_EQUAL_INT16(21, bar.h);
    TEST_ASSERT_EQUAL_HEX16(settings.get().colorArrowSide, bar.color);
    assertTriangle(0, 564, 2, 503, 70, 625, 70, TFT_DARKGREY);
    assertTriangle(1, 505, 94, 532, 73, 532, 115, settings.get().colorArrowSide);
    assertTriangle(2, 623, 94, 596, 73, 596, 115, settings.get().colorArrowSide);
    assertTriangle(3, 564, 146, 503, 118, 625, 118, TFT_DARKGREY);
}

void test_multiple_spec_direction_bits_activate_every_glyph() {
    const Direction directions = directionBitmapFromDisplayBits(
        static_cast<uint8_t>(kSpecDirectionFront | kSpecDirectionSide | kSpecDirectionRear));
    TEST_ASSERT_EQUAL_INT(DIR_FRONT | DIR_SIDE | DIR_REAR, directions);

    display.drawDirectionArrow(directions, false, 0);

    TEST_ASSERT_EQUAL_HEX16(settings.get().colorArrowFront, canvas()->fillTriangleCalls[0].color);
    TEST_ASSERT_EQUAL_HEX16(settings.get().colorArrowSide, canvas()->fillTriangleCalls[1].color);
    TEST_ASSERT_EQUAL_HEX16(settings.get().colorArrowSide, canvas()->fillTriangleCalls[2].color);
    TEST_ASSERT_EQUAL_HEX16(settings.get().colorArrowRear, canvas()->fillTriangleCalls[3].color);
}

void test_unchanged_direction_produces_no_paint_commands() {
    const AlertData alert = radarAlertFromSpecFields(kSpecDirectionFront);
    drawRadarDirection(alert);
    resetRecordedOutput();

    drawRadarDirection(alert);

    TEST_ASSERT_EQUAL_UINT(0, canvas()->fillTriangleCalls.size());
    TEST_ASSERT_EQUAL_UINT(0, canvas()->fillRectCalls.size());
    TEST_ASSERT_EQUAL_UINT(0, canvas()->drawLineCalls.size());
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_front_spec_direction_draws_the_apex_up_glyph);
    RUN_TEST(test_rear_spec_direction_draws_the_apex_down_glyph);
    RUN_TEST(test_side_spec_direction_activates_the_bar_and_side_heads_only);
    RUN_TEST(test_multiple_spec_direction_bits_activate_every_glyph);
    RUN_TEST(test_unchanged_direction_produces_no_paint_commands);
    return UNITY_END();
}
