// Live-frame integration: real frame dispatch, counter, cards, bands, bars and
// arrows. Unchanged frequency/status peripherals are inert test dependencies;
// the physical panel boundary records full versus regional transfer requests.
#define DISPLAY_WAVESHARE_349 1

#include <unity.h>
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>
#include "../mocks/display_driver.h"
#include "../mocks/Arduino.h"
#include "../mocks/settings.h"
#include "../mocks/packet_parser.h"
#include "../mocks/battery_manager.h"

unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
SerialClass Serial;

// display_update also contains resting-mode code. Keep its disconnected
// peripherals inert without importing the WiFi runtime into this display test.
#define WIFI_MANAGER_H
class WiFiManager {
  public:
    bool isWifiServiceActive() const { return false; }
    bool isConnected() const { return false; }
    bool isReconnectGaveUp() const { return false; }
};
#include "../../src/modules/gps/gps_runtime_module.h"
GpsRuntimeStatus GpsRuntimeModule::snapshot(uint32_t) const { return {}; }

#include "../../src/display.h"

V1Display* g_displayInstance = nullptr;
SettingsManager settings;
std::vector<DisplayLayout::DisplayRect> regionalTransfers;

class RecordingCanvas : public Arduino_Canvas {
  public:
    RecordingCanvas() : Arduino_Canvas(SCREEN_WIDTH, SCREEN_HEIGHT, nullptr) {}
    std::vector<std::string> printed;
    void print(const char* text) override { printed.emplace_back(text); }
};

V1Display::V1Display(SettingsManager& injectedSettings) : settings_(injectedSettings) {
    currentPalette_ = ColorThemes::STANDARD();
    currentPalette_.colorMuted = settings_.get().colorMuted;
    currentPalette_.colorPersisted = settings_.get().colorPersisted;
    g_displayInstance = this;
}
V1Display::~V1Display() = default;
void V1Display::flushRegion(int16_t x, int16_t y, int16_t w, int16_t h) {
    regionalTransfers.push_back({x, y, w, h});
}
void V1Display::drawBaseFrame() {
    tft_->fillScreen(TFT_BLACK);
    elementCaches_.invalidateAll();
}
void V1Display::drawFrequency(uint32_t, Band, bool, bool) {}
void V1Display::drawVolumeZeroWarning() {}
void V1Display::drawVolumeIndicator(uint8_t, uint8_t) {}
void V1Display::drawRssiIndicator(int) {}
void V1Display::drawWiFiIndicator() {}
void V1Display::drawBatteryIndicator() {}
void V1Display::drawBLEProxyIndicator() {}
void V1Display::drawObdIndicator() {}
void V1Display::drawGpsIndicator() { dirty_.gpsIndicator = false; }
void V1Display::drawAlpIndicator() {}
void V1Display::drawProfileIndicator(int) {}
void V1Display::syncTopIndicators(uint32_t) {}
bool V1Display::hasFreshBleContext(uint32_t) const { return false; }
void V1Display::showStealth(float, bool) {}
const char* V1Display::bandToString(Band band) {
    switch (band) {
    case BAND_KA: return "Ka";
    case BAND_K: return "K";
    case BAND_X: return "X";
    default: return "";
    }
}
uint16_t V1Display::getBandColor(Band band) {
    const auto& s = settings_.get();
    switch (band) {
    case BAND_KA: return s.colorBandKa;
    case BAND_K: return s.colorBandK;
    case BAND_X: return s.colorBandX;
    default: return currentPalette_.text;
    }
}
bool DisplayFontManager::getTopCounterBounds(char, bool, int& xMin, int& xMax) {
    xMin = 0;
    xMax = 20;
    return true;
}

#include "../../src/display_top_counter.cpp"
#include "../../src/display_cards.cpp"
#include "../../src/display_bands.cpp"
#include "../../src/display_arrow.cpp"
#include "../../src/display_update.cpp"

V1Display display(settings);

namespace {
RecordingCanvas* canvas() { return static_cast<RecordingCanvas*>(display.testCanvas()); }

void clearObservations() {
    canvas()->resetCounters();
    canvas()->printed.clear();
    regionalTransfers.clear();
    display.ut_fontMgr().segment7.resetRecordedCalls();
}

DisplayState stateFor(const AlertData& primary, char counter, uint8_t mainBars) {
    DisplayState state;
    state.activeBands = primary.band;
    state.arrows = primary.direction;
    state.priorityArrow = primary.direction;
    state.signalBars = mainBars;
    state.bogeyCounterChar = state.bogeyCounterChar2 = counter;
    state.bogeyCounterByte = state.bogeyCounterByte2 = counter == '1' ? 0x06 : 0x5b;
    return state;
}

void showLive(const AlertData& primary, const AlertData* secondary, uint8_t mainBars) {
    RenderFrame frame;
    frame.primaryKind = RenderFramePrimaryKind::V1_LIVE;
    frame.v1Priority = primary;
    frame.primaryState = stateFor(primary, secondary ? '2' : '1', mainBars);
    if (secondary) {
        frame.cardCount = 1;
        frame.cards[0].kind = RenderFrameCard::Kind::V1;
        frame.cards[0].v1Alert = *secondary;
    }
    display.renderFrame(frame);
}

void assertXPaintAndSafeDispatch(const AlertData& primary, uint8_t mainBars) {
    const AlertData x = AlertData::create(BAND_X, DIR_REAR, 0, 2, 10525, true, false);
    clearObservations();
    mockMillis += 333;
    showLive(primary, &x, mainBars);

    // Establish the literal paint request before asserting panel dispatch.
    TEST_ASSERT_EQUAL_STRING("2", display.ut_elementCaches().topCounter.lastText);
    TEST_ASSERT_EQUAL_STRING("2", display.ut_fontMgr().segment7.lastPrinted);
    TEST_ASSERT_EQUAL_INT(1, display.ut_fontMgr().segment7.printfCount);
    TEST_ASSERT_EQUAL_INT(BAND_X, display.ut_elementCaches().cards.lastDrawnPositions[0].band);
    TEST_ASSERT_EQUAL_UINT32(10525, display.ut_elementCaches().cards.lastDrawnPositions[0].frequency);
    TEST_ASSERT_FALSE(display.ut_elementCaches().cards.lastDrawnPositions[0].isGraced);
    TEST_ASSERT_TRUE(std::find(canvas()->printed.begin(), canvas()->printed.end(), "X") != canvas()->printed.end());
    TEST_ASSERT_TRUE(std::find(canvas()->printed.begin(), canvas()->printed.end(), "10.525") != canvas()->printed.end());
    TEST_ASSERT_EQUAL_UINT(1, canvas()->fillRoundRectCalls.size());
    const int changedFullTransfers = canvas()->getFlushCount();
    const auto changedRegions = regionalTransfers;

    clearObservations();
    mockMillis += 333;
    showLive(primary, &x, mainBars);
    TEST_ASSERT_EQUAL_INT(0, canvas()->getFlushCount());
    TEST_ASSERT_TRUE(regionalTransfers.empty());
    TEST_ASSERT_EQUAL_INT(0, display.ut_fontMgr().segment7.printfCount);
    TEST_ASSERT_TRUE(canvas()->fillRoundRectCalls.empty());
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, changedFullTransfers,
        "new live counter/card pixels must use the full-canvas panel transfer");
    TEST_ASSERT_TRUE(changedRegions.empty());
}
} // namespace

void setUp() {
    mockMillis = mockMicros = 1000;
    settings = SettingsManager{};
    settings.slotAlertPersistSec[0] = 2;
    display.~V1Display();
    new (&display) V1Display(settings);
    display.setTestCanvas(new RecordingCanvas());
    display.ut_fontMgr().segment7Ready = true;
    clearObservations();
}
void tearDown() {}

void test_event0007_new_x_with_unchanged_ka_primary_paints_and_full_flushes() {
    const auto ka = AlertData::create(BAND_KA, DIR_SIDE, 6, 0, 34700, true, true);
    showLive(ka, nullptr, 6);
    TEST_ASSERT_EQUAL_INT(1, canvas()->getFlushCount());
    assertXPaintAndSafeDispatch(ka, 6);
}

void test_event0010_new_x_with_unchanged_k_primary_paints_and_full_flushes() {
    const auto ka = AlertData::create(BAND_KA, DIR_SIDE, 6, 0, 34700, true, true);
    const auto k = AlertData::create(BAND_K, DIR_FRONT, 4, 0, 24150, true, true);
    mockMillis = 43000;
    showLive(ka, nullptr, 6);
    mockMillis += 333;
    showLive(k, nullptr, 4);
    TEST_ASSERT_EQUAL_INT(BAND_KA, display.ut_elementCaches().cards.lastDrawnPositions[0].band);
    TEST_ASSERT_TRUE(display.ut_elementCaches().cards.lastDrawnPositions[0].isGraced);

    // The previous Ka must leave the framebuffer after the configured two
    // seconds while K remains live. Its absent cache state alone cannot prove
    // that the physical panel received the removal.
    clearObservations();
    mockMillis += 2333;
    showLive(k, nullptr, 4);
    TEST_ASSERT_EQUAL_INT(BAND_NONE, display.ut_elementCaches().cards.lastDrawnPositions[0].band);
    TEST_ASSERT_EQUAL_INT(0, display.ut_elementCaches().cards.lastDrawnCount);
    TEST_ASSERT_FALSE(canvas()->fillRectCalls.empty());
    TEST_ASSERT_EQUAL_INT(1, canvas()->getFlushCount());
    TEST_ASSERT_TRUE(regionalTransfers.empty());
    mockMillis = 51000;
    assertXPaintAndSafeDispatch(k, 4);
}

void test_live_counter_only_change_full_flushes_then_cache_hit_skips() {
    const auto ka = AlertData::create(BAND_KA, DIR_SIDE, 6, 0, 34700, true, true);
    display.update(ka, &ka, 1, stateFor(ka, '1', 6));
    clearObservations();
    display.update(ka, &ka, 1, stateFor(ka, '2', 6));
    TEST_ASSERT_EQUAL_STRING("2", display.ut_fontMgr().segment7.lastPrinted);
    TEST_ASSERT_EQUAL_INT(1, canvas()->getFlushCount());
    TEST_ASSERT_TRUE(regionalTransfers.empty());
    clearObservations();
    display.update(ka, &ka, 1, stateFor(ka, '2', 6));
    TEST_ASSERT_EQUAL_INT(0, canvas()->getFlushCount());
    TEST_ASSERT_TRUE(regionalTransfers.empty());
}

void test_live_pending_draw_full_flushes_even_when_frame_itself_is_unchanged() {
    const auto ka = AlertData::create(BAND_KA, DIR_SIDE, 6, 0, 34700, true, true);
    display.update(ka, &ka, 1, stateFor(ka, '1', 6));
    clearObservations();
    display.ut_drawTopCounterPair('2', false, false);
    TEST_ASSERT_FALSE(display.ut_drawnRegionEmpty());
    display.update(ka, &ka, 1, stateFor(ka, '2', 6));
    TEST_ASSERT_EQUAL_INT(1, canvas()->getFlushCount());
    TEST_ASSERT_TRUE(regionalTransfers.empty());
    TEST_ASSERT_TRUE(display.ut_drawnRegionEmpty());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_event0007_new_x_with_unchanged_ka_primary_paints_and_full_flushes);
    RUN_TEST(test_event0010_new_x_with_unchanged_k_primary_paints_and_full_flushes);
    RUN_TEST(test_live_counter_only_change_full_flushes_then_cache_hit_skips);
    RUN_TEST(test_live_pending_draw_full_flushes_even_when_frame_itself_is_unchanged);
    return UNITY_END();
}
