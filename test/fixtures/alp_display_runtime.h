// Real ALP parser, owner pipeline, event setter, frame dispatch and SCAN painter.
// Clock/BLE/settings and peripheral I/O are substitutes; the canvas/font record
// paint and transfer requests, not physical panel pixels or speaker output.
#pragma once
#define DISPLAY_WAVESHARE_349 1
#include <unity.h>
#include <vector>
#include "../mocks/Arduino.h"
#include "../mocks/display_driver.h"
#include "../mocks/settings.h"
#include "../mocks/ble_client.h"
#include "../mocks/battery_manager.h"

#define WIFI_MANAGER_H
class WiFiManager {
  public:
    bool isWifiServiceActive() const { return false; }
    bool isConnected() const { return false; }
    bool isReconnectGaveUp() const { return false; }
};
#include "../../src/display.h"
#include "../../src/modules/gps/gps_runtime_module.h"
#include "../../src/modules/alp/alp_runtime_module.cpp"
#include "../../src/packet_parser.cpp"
#include "../../src/packet_parser_alerts.cpp"
#include "../../src/modules/voice/voice_module.cpp"
#include "../../src/modules/alert_persistence/alert_persistence_module.cpp"
#include "../../src/modules/alp/alp_event_latch.cpp"
#include "../../src/modules/display/render_frame_composer.cpp"
#include "../../src/modules/display/display_pipeline_module.cpp"
#include "../../src/display_indicators.cpp"
#include "../../src/display_top_counter.cpp"
#include "../../src/display_cards.cpp"
#include "../../src/display_bands.cpp"
#include "../../src/display_arrow.cpp"
#include "../../src/display_update.cpp"
#include "../../src/display_screens.cpp"

SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
V1Display* g_displayInstance = nullptr;
static Band frequencyBand = BAND_NONE;
static std::vector<DisplayLayout::DisplayRect> regionTransfers;

V1Display::V1Display(SettingsManager& settings) : settings_(settings) {
    currentPalette_ = ColorThemes::STANDARD();
    g_displayInstance = this;
}
V1Display::~V1Display() = default;
void V1Display::flushRegion(int16_t x, int16_t y, int16_t w, int16_t h) {
    regionTransfers.push_back({x, y, w, h});
}
void V1Display::drawFrequency(uint32_t, Band band, bool, uint8_t) { frequencyBand = band; }
void V1Display::drawVolumeZeroWarning() {}
#ifndef ALP_DISPLAY_REAL_STATUS
void V1Display::drawVolumeIndicator(uint8_t, uint8_t) {}
void V1Display::drawRssiIndicator(int) {}
void V1Display::drawWiFiIndicator() {}
void V1Display::drawBatteryIndicator() {}
void V1Display::drawBLEProxyIndicator() {}
void V1Display::drawProfileIndicator(int) {}
#endif
void V1Display::setBrightness(uint8_t) {}
#ifndef ALP_DISPLAY_REAL_STATUS
bool V1Display::hasFreshBleContext(uint32_t) const { return false; }
#endif
const char* V1Display::bandToString(Band band) {
    switch (band) {
    case BAND_KA: return "Ka";
    case BAND_K: return "K";
    case BAND_X: return "X";
    default: return "";
    }
}
uint16_t V1Display::getBandColor(Band) { return TFT_WHITE; }
bool DisplayFontManager::getTopCounterBounds(char, bool, int& min, int& max) {
    min = 0; max = 20; return true;
}
GpsRuntimeStatus GpsRuntimeModule::snapshot(uint32_t) const { return {}; }
ObdRuntimeStatus ObdRuntimeModule::snapshot(uint32_t) const { return {}; }
void QuietCoordinatorModule::syncCommittedState() { TEST_FAIL_MESSAGE("unexpected quiet owner"); }
AudioPlaybackResult try_play_frequency_voice(AlertBand, uint16_t, AlertDirection, VoiceAlertMode, bool, uint8_t, AudioPlaybackReceipt*) {
    TEST_FAIL_MESSAGE("disconnected V1 must not speak"); return AudioPlaybackResult::Unavailable;
}
AudioPlaybackResult try_play_direction_only(AlertDirection, uint8_t, AudioPlaybackReceipt*) {
    TEST_FAIL_MESSAGE("disconnected V1 must not speak"); return AudioPlaybackResult::Unavailable;
}
AudioPlaybackResult try_play_threat_escalation(AlertBand, uint16_t, AlertDirection, uint8_t, uint8_t, uint8_t, uint8_t,
                                               VoiceAlertMode, bool, bool, AudioPlaybackReceipt*) {
    TEST_FAIL_MESSAGE("disconnected V1 must not speak"); return AudioPlaybackResult::Unavailable;
}

void setUp() { mockMillis = mockMicros = 0; frequencyBand = BAND_NONE; regionTransfers.clear(); }
void tearDown() { g_displayInstance = nullptr; }

namespace {
struct Runtime {
    SettingsManager settings;
    V1Display display{settings};
    AlpRuntimeModule alp;
    PacketParser parser;
    V1BLEClient ble;
    AlertPersistenceModule persistence;
    VoiceModule voice;
    AlpEventLatch latch;
    SpeedSourceSelector speed;
    DisplayMode mode = DisplayMode::IDLE;
    DisplayPipelineModule pipeline;

    Runtime(bool connected = false, uint8_t persistSec = 0) {
        ble.setConnected(connected);
        settings.alpAlertPersistSec = persistSec;
        voice.begin(&settings);
        display.setTestCanvas(new Arduino_Canvas(SCREEN_WIDTH, SCREEN_HEIGHT, nullptr));
        display.setAlpRuntimeModule(&alp);
        display.ut_fontMgr().segment7Ready = true;
        DisplayPipelineDependencies deps;
        deps.displayMode = &mode;
        deps.display = &display;
        deps.parser = &parser;
        deps.settings = &settings;
        deps.ble = &ble;
        deps.alertPersistence = &persistence;
        deps.voice = &voice;
        deps.alp = &alp;
        deps.alpLatch = &latch;
        deps.speedSelector = &speed;
        pipeline.begin(deps);
        alp.begin(true);
    }

    void clearPaint() {
        display.testCanvas()->resetCounters();
        display.ut_fontMgr().segment7.resetRecordedCalls();
        frequencyBand = BAND_NONE;
        regionTransfers.clear();
    }
    void feed(uint32_t now, uint8_t a, uint8_t b, uint8_t c) {
        const uint8_t bytes[]{a, b, c, alpChecksum(a, b, c)};
        mockMillis = now;
        alp.testInjectBytes(bytes, sizeof(bytes));
        alp.testSetLastUartByteMs(now);
        alp.process(now);
        pipeline.handleParsed(now);
    }
    void live() {
        feed(100, 0xB0, 0x03, 0);
        feed(200, 0x98, 0, 0xE3);
        feed(210, 0xC8, 0, 0xD5);
        TEST_ASSERT_EQUAL(DisplayMode::LIVE, mode);
        TEST_ASSERT_TRUE(display.ut_alpHasLaserEvent());
        TEST_ASSERT_EQUAL_STRING("PL3", display.ut_alpFreqText());
        TEST_ASSERT_EQUAL(BAND_LASER, frequencyBand);
    }
    void silence() {
        mockMillis = 3211;
        alp.process(mockMillis);
        TEST_ASSERT_FALSE(alp.currentEvent().active);
        clearPaint();
        pipeline.handleParsed(mockMillis);
    }
    void scan() {
        TEST_ASSERT_EQUAL(DisplayMode::IDLE, mode);
        TEST_ASSERT_EQUAL_STRING("SCAN", display.ut_fontMgr().segment7.lastPrinted);
        TEST_ASSERT_EQUAL_INT(1, display.testCanvas()->getFlushCount());
        TEST_ASSERT_TRUE(regionTransfers.empty());
        TEST_ASSERT_FALSE(display.ut_alpHasLaserEvent());
        TEST_ASSERT_FALSE(display.ut_alpFreqOverride());
        TEST_ASSERT_EQUAL_STRING("", display.ut_alpFreqText());
        TEST_ASSERT_EQUAL(BAND_NONE, display.ut_elementCaches().bands.lastMask);
        TEST_ASSERT_FALSE(display.ut_elementCaches().arrow.showFront);
        TEST_ASSERT_FALSE(display.ut_elementCaches().arrow.showRear);
    }
    void tick(uint32_t now) {
        mockMillis = now;
        if (pipeline.consumeAlpPresentationRefreshDue(now)) pipeline.handleParsed(now);
    }
};
} // namespace
