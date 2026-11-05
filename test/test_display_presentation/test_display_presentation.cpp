// Actual presentation source, resolution and status painters. Source observations
// and the canvas are scripted; this does not model physical transport/panel I/O.
#define DISPLAY_WAVESHARE_349 1
#include <unity.h>
#include <array>
#include <memory>
#include <string>
#include <vector>
#include "../mocks/Arduino.h"
#include "../mocks/display_driver.h"
#include "../mocks/settings.h"
#include "../mocks/battery_manager.h"
#include "../../src/runtime_presentation_source.h"
#include "../../src/modules/obd/obd_runtime_module.h"
#include "../../src/modules/alp/alp_runtime_module.h"
#include "../../src/modules/gps/gps_runtime_module.h"
#include "../../src/modules/gps/gps_runtime_module.cpp"
#include "../../src/modules/quiet/quiet_coordinator_module.h"

#define WIFI_MANAGER_H
class WiFiManager {
  public:
    bool active = false, connected = false, gaveUp = false;
    mutable unsigned activeReads = 0, connectedReads = 0, gaveUpReads = 0;
    bool isWifiServiceActive() const { ++activeReads; return active; }
    bool isConnected() const { ++connectedReads; return connected; }
    bool isReconnectGaveUp() const { ++gaveUpReads; return gaveUp; }
};
#define private public
#include "../../src/display.h"
#undef private
#include "../../src/runtime_presentation_source.cpp"
#include "../../src/display_presentation.cpp"
#include "../../src/display_status_bar.cpp"
#include "../../src/display_indicators.cpp"
#include "../../src/display_top_counter.cpp"
#include "../../src/display_cards.cpp"
#include "../../src/display_bands.cpp"
#include "../../src/display_arrow.cpp"
#include "../../src/display_frequency.cpp"
#include "../../src/display_frequency_digit_atlas.cpp"
#include "../../src/display_frequency_raster_cache.cpp"
#include "../../src/display_update.cpp"
#include "../../src/display_screens.cpp"
#include "../../src/modules/power/power_module.cpp"

SerialClass Serial;
unsigned long mockMillis = 0, mockMicros = 0;
V1Display* g_displayInstance = nullptr;
V1Display::V1Display() { currentPalette_ = ColorThemes::STANDARD(); }
V1Display::~V1Display() = default;
const char* V1Display::bandToString(Band) {
    TEST_FAIL_MESSAGE("unexpected secondary card in status fixture");
    return "";
}
void V1Display::flushRegion(int16_t, int16_t, int16_t, int16_t) {}
void V1Display::setBrightness(uint8_t) { TEST_FAIL_MESSAGE("unexpected boot splash in status fixture"); }
void V1Display::clear(const DisplayPaintContext&) { TEST_FAIL_MESSAGE("unexpected shutdown in status fixture"); }
void V1Display::flush() { TEST_FAIL_MESSAGE("unexpected aborted shutdown in status fixture"); }
uint16_t V1Display::getBandColor(const ResolvedPaintStatus& paint, Band band) {
    const auto& s = paint.context.style.settings;
    switch (band) {
    case BAND_LASER: return s.colorBandL;
    case BAND_KA: return s.colorBandKa;
    case BAND_K: case BAND_KU: return s.colorBandK;
    case BAND_X: return s.colorBandX;
    default: return currentPalette_.text;
    }
}
bool DisplayFontManager::getTopCounterBounds(char, bool, int&, int&) { return false; }

static ObdRuntimeStatus obdObservation;
static AlpStatus alpObservation;
static unsigned obdReads, alpReads;
static uint32_t obdReadAt;
ObdRuntimeStatus ObdRuntimeModule::snapshot(uint32_t now) const {
    ++obdReads; obdReadAt = now; return obdObservation;
}
AlpStatus AlpRuntimeModule::snapshot() const { ++alpReads; return alpObservation; }
const char* alpGunAbbrev(AlpGunType gun) {
    TEST_ASSERT_EQUAL(AlpGunType::PL3_PROLITE, gun);
    return "PL3";
}

struct Fixture {
    SettingsManager settings;
    RuntimePresentationSource source{settings};
    ObdRuntimeModule obd;
    AlpRuntimeModule alp;
    BatteryManager battery;
    WiFiManager wifi;
    GpsRuntimeModule gps;
    QuietCoordinatorModule quiet;
    V1Display display;
    Fixture(bool bind = true) {
        display.setTestCanvas(new Arduino_Canvas(SCREEN_WIDTH, SCREEN_HEIGHT, nullptr));
        if (bind) source.bind(obd, alp, battery, wifi, gps, quiet);
    }
    void batteryAt(uint16_t millivolts, uint8_t percent = 80) {
        battery.setHasBattery(true);
        battery.setVoltage((millivolts + 0.25f) / 1000.0f);
        TEST_ASSERT_EQUAL_UINT16(millivolts, battery.getVoltageMillivolts());
        battery.setBatteryPercent(percent);
    }
    void paintBattery(uint32_t now) {
        mockMillis = now;
        const auto inputs = source.captureForPaint();
        display.drawBatteryIndicator(inputs.context());
    }
};

void setUp() {
    mockMillis = mockMicros = 0;
    obdObservation = {}; alpObservation = {};
    obdReads = alpReads = 0;
    obdReadAt = 0;
}
void tearDown() {}

void test_unbound_source_is_unavailable_and_preserves_original_ble_age() {
    Fixture f(false);
    f.source.noteBleSample({true, true, -73, -89}, true, 50);
    mockMillis = 2000;
    const auto inputs = f.source.captureForPaint();
    TEST_ASSERT_EQUAL_UINT32(2000, inputs.nowMs);
    TEST_ASSERT_EQUAL_UINT32(50, inputs.status.bleSampledAtMs);
    TEST_ASSERT_EQUAL_UINT32(50, inputs.status.bleBadgeSampledAtMs);
    TEST_ASSERT_FALSE(inputs.status.obdEnabled);
    TEST_ASSERT_FALSE(inputs.status.alpEnabled);
    TEST_ASSERT_FALSE(inputs.status.gpsEnabled);
    TEST_ASSERT_FALSE(inputs.status.hasBattery);
    TEST_ASSERT_FALSE(inputs.status.wifiServiceActive);
    TEST_ASSERT_EQUAL_UINT32(0, obdReads + alpReads);
    const auto context = inputs.context();
    const auto paint = f.display.ut_resolvePaintStatus(context);
    TEST_ASSERT_FALSE(paint.bleFresh);
    TEST_ASSERT_EQUAL_INT(-73, paint.v1Rssi);
    TEST_ASSERT_EQUAL_INT(-89, paint.proxyRssi);
}

void test_capture_reads_each_observation_once_without_refreshing_or_starting_producers() {
    Fixture f;
    obdObservation.enabled = true; obdObservation.connected = false;
    obdObservation.manualScanPending = true;
    f.gps.begin(true);
    f.gps.setScaffoldSample(20.0f, true, 11, 0.9f, 900);
    alpObservation.state = AlpState::IDLE; alpObservation.lastHbByte1 = 4;
    f.wifi.active = true; f.wifi.gaveUp = true;
    f.batteryAt(3900, 61);
    const_cast<QuietPresentationState&>(f.quiet.getPresentationState()).speedVolZeroActive = true;
    f.source.setObdAttention(true);
    f.source.noteBleSample({true, false, -75, 0}, false, 123);
    const auto status = f.source.capture(997);
    TEST_ASSERT_EQUAL_UINT32(1, obdReads); TEST_ASSERT_EQUAL_UINT32(1, alpReads);
    TEST_ASSERT_EQUAL_UINT32(997, obdReadAt);
    TEST_ASSERT_EQUAL_UINT32(1, f.wifi.activeReads); TEST_ASSERT_EQUAL_UINT32(1, f.wifi.connectedReads); TEST_ASSERT_EQUAL_UINT32(1, f.wifi.gaveUpReads);
    TEST_ASSERT_EQUAL_UINT32(123, status.bleSampledAtMs);
    TEST_ASSERT_TRUE(status.obdAttention); TEST_ASSERT_TRUE(status.obdManualScanPending);
    TEST_ASSERT_TRUE(status.gpsStableFix); TEST_ASSERT_EQUAL_UINT8(11, status.gpsSatellites);
    TEST_ASSERT_TRUE(status.alpEnabled); TEST_ASSERT_EQUAL_UINT8(4, status.alpHeartbeat);
    TEST_ASSERT_TRUE(status.wifiServiceActive); TEST_ASSERT_TRUE(status.wifiGaveUp);
    TEST_ASSERT_TRUE(status.speedVolZeroActive);
    TEST_ASSERT_EQUAL_UINT8(61, status.batteryPercent);
    TEST_ASSERT_EQUAL_UINT16(3900, status.batteryMillivolts);
}

void test_resolution_matches_literal_gps_obd_wifi_and_ble_boundaries() {
    Fixture f;
    auto inputs = f.source.captureForPaint();
    inputs.status.gpsEnabled = true;
    inputs.status.gpsSatellites = 12;
    inputs.status.obdEnabled = true;
    inputs.status.obdAttention = true;
    inputs.status.wifiServiceActive = true;
    inputs.status.wifiGaveUp = true;
    inputs.status.ble = {true, true, -75, -90};
    inputs.status.bleProxyEnabled = inputs.status.bleProxyClientConnected = true;
    inputs.status.bleSampledAtMs = inputs.status.bleBadgeSampledAtMs = 100;
    inputs.nowMs = 1100;
    auto context = inputs.context();
    auto paint = f.display.ut_resolvePaintStatus(context);
    TEST_ASSERT_FALSE(paint.gpsShown); TEST_ASSERT_EQUAL_UINT8(0, paint.gpsSatellites);
    TEST_ASSERT_TRUE(paint.obdAttention); TEST_ASSERT_EQUAL_HEX16(0xf800, paint.obdColor);
    TEST_ASSERT_TRUE(paint.wifiShown); TEST_ASSERT_EQUAL_HEX16(0xf800, paint.wifiColor);
    TEST_ASSERT_TRUE(paint.bleFresh);
    TEST_ASSERT_EQUAL_HEX16(COLOR_GREEN, paint.v1RssiColor);
    TEST_ASSERT_EQUAL_HEX16(COLOR_YELLOW, paint.proxyRssiColor);
    inputs.nowMs = 1101;
    inputs.status.gpsStableFix = true;
    inputs.status.obdConnected = true;
    inputs.status.wifiConnected = true;
    context.nowMs = inputs.nowMs; const auto changedPaint = f.display.ut_resolvePaintStatus(context);
    TEST_ASSERT_FALSE(changedPaint.bleFresh); TEST_ASSERT_TRUE(changedPaint.gpsShown);
    TEST_ASSERT_EQUAL_UINT8(12, changedPaint.gpsSatellites);
    TEST_ASSERT_FALSE(changedPaint.obdAttention); TEST_ASSERT_EQUAL_HEX16(f.settings.get().colorObd, changedPaint.obdColor);
    TEST_ASSERT_EQUAL_HEX16(dimColor(f.settings.get().colorWiFiConnected, 85), changedPaint.wifiColor);
    inputs.status.bleSampledAtMs = 0; inputs.nowMs = 0;
    context.nowMs = inputs.nowMs; TEST_ASSERT_FALSE(f.display.ut_resolvePaintStatus(context).bleFresh);
    inputs.status.bleSampledAtMs = UINT32_MAX - 50; inputs.nowMs = 49;
    context.nowMs = inputs.nowMs; TEST_ASSERT_TRUE(f.display.ut_resolvePaintStatus(context).bleFresh);
}

void test_profile_selection_is_paint_free_and_delayed_paint_does_not_extend_reveal() {
    Fixture f;
    f.settings.settings.hideProfileIndicator = true;
    f.display.setProfileIndicatorSlot(1, 100);
    TEST_ASSERT_TRUE(f.display.ut_drawnRegionEmpty());
    mockMillis = 3099;
    auto inputs = f.source.captureForPaint(); auto context = inputs.context();
    TEST_ASSERT_FALSE(f.display.ut_resolvePaintStatus(context).profileHidden);
    mockMillis = 3100;
    inputs.nowMs = 3100; context.nowMs = inputs.nowMs;
    TEST_ASSERT_TRUE(f.display.ut_resolvePaintStatus(context).profileHidden);
    f.display.setProfileIndicatorSlot(1, 3100);
    TEST_ASSERT_TRUE(f.display.ut_resolvePaintStatus(context).profileHidden);
    f.display.setProfileIndicatorSlot(2, 3200);
    f.display.setProfileIndicatorSlot(1, 3300);
    inputs.nowMs = 6299; context.nowMs = inputs.nowMs;
    TEST_ASSERT_FALSE(f.display.ut_resolvePaintStatus(context).profileHidden);
    inputs.nowMs = 6300; context.nowMs = inputs.nowMs;
    TEST_ASSERT_TRUE(f.display.ut_resolvePaintStatus(context).profileHidden);
}

void test_profile_selection_zero_wrap_and_preview_restore_use_one_timer() {
    Fixture f;
    f.settings.settings.hideProfileIndicator = true;
    f.display.setProfileIndicatorSlot(0, 0);
    auto inputs = f.source.captureForPaint(); auto context = inputs.context();
    TEST_ASSERT_FALSE(f.display.ut_resolvePaintStatus(context).profileHidden);
    inputs.nowMs = 3000; context.nowMs = inputs.nowMs;
    TEST_ASSERT_TRUE(f.display.ut_resolvePaintStatus(context).profileHidden);
    f.display.setProfileIndicatorSlot(2, UINT32_MAX - 100);
    inputs.nowMs = 2898; context.nowMs = inputs.nowMs;
    TEST_ASSERT_FALSE(f.display.ut_resolvePaintStatus(context).profileHidden);
    inputs.nowMs = 2899; context.nowMs = inputs.nowMs;
    TEST_ASSERT_TRUE(f.display.ut_resolvePaintStatus(context).profileHidden);
    // Preview selects 1, then normal feedback selects 0. Neither paints here.
    f.display.setProfileIndicatorSlot(1, 4000);
    f.display.setProfileIndicatorSlot(0, 4100);
    inputs.nowMs = 7100; context.nowMs = inputs.nowMs; context.cardPersistence = CardPersistencePolicy::PreviewImmediate;
    const auto paint = f.display.ut_resolvePaintStatus(context);
    TEST_ASSERT_EQUAL_INT(0, paint.profileSlot); TEST_ASSERT_TRUE(paint.profileHidden);
}

void test_battery_hysteresis_commits_only_when_painted_and_uses_exact_thresholds() {
    Fixture f;
    f.batteryAt(4126);
    auto inputs = f.source.captureForPaint(); auto context = inputs.context();
    TEST_ASSERT_FALSE(f.display.ut_resolvePaintStatus(context).batteryShown);
    // Resolution for a non-battery paint must not alter the old hysteresis state.
    f.batteryAt(4100); f.paintBattery(10);
    TEST_ASSERT_TRUE(f.display.ut_elementCaches().battery.lastIconShown);
    f.batteryAt(4126); f.paintBattery(20);
    TEST_ASSERT_FALSE(f.display.ut_elementCaches().battery.lastIconShown);
    f.batteryAt(4095); f.paintBattery(30);
    TEST_ASSERT_FALSE(f.display.ut_elementCaches().battery.lastIconShown);
    f.batteryAt(4094); f.paintBattery(40);
    TEST_ASSERT_TRUE(f.display.ut_elementCaches().battery.lastIconShown);
    f.batteryAt(4125); f.paintBattery(50);
    TEST_ASSERT_TRUE(f.display.ut_elementCaches().battery.lastIconShown);
}

void test_battery_percent_cadence_and_profile_overlap_share_resolved_values() {
    Fixture f;
    f.settings.settings.showBatteryPercent = true;
    f.batteryAt(3900, 61); f.paintBattery(100);
    TEST_ASSERT_TRUE(f.display.ut_elementCaches().battery.lastPctVisible);
    f.display.ut_resetDrawnRegion(); f.paintBattery(60099);
    TEST_ASSERT_TRUE(f.display.ut_drawnRegionEmpty());
    f.paintBattery(60100); TEST_ASSERT_FALSE(f.display.ut_drawnRegionEmpty());
    f.display.ut_resetDrawnRegion();
    f.display.setProfileIndicatorSlot(1, 60100);
    const auto inputs = f.source.captureForPaint();
    f.display.drawProfileIndicator(inputs.context());
    TEST_ASSERT_TRUE(f.display.ut_elementCaches().battery.lastPctVisible);
    TEST_ASSERT_EQUAL_INT(61, f.display.ut_elementCaches().battery.lastPctDrawn);
    f.display.ut_resetDrawnRegion(); f.display.drawProfileIndicator(inputs.context());
    TEST_ASSERT_TRUE(f.display.ut_drawnRegionEmpty());
}

void test_status_only_paint_preserves_same_session_gun_but_new_empty_frame_clears_it() {
    Fixture f;
    auto inputs = f.source.captureForPaint(); auto context = inputs.context();
    RenderFrame frame;
    frame.primaryKind = RenderFramePrimaryKind::ALP_LIVE;
    frame.alpPrimary.active = true; frame.alpPrimary.sessionGeneration = 7;
    frame.alpPrimary.gun = AlpGunType::PL3_PROLITE;
    f.display.ut_resolvePaintStatus(context, &frame);
    f.display.drawBatteryIndicator(context);
    TEST_ASSERT_EQUAL_STRING("PL3", f.display.ut_alpFreqText());
    frame.alpPrimary.gun = AlpGunType::UNKNOWN;
    const auto retained = f.display.ut_resolvePaintStatus(context, &frame);
    TEST_ASSERT_TRUE(retained.hasGunLabel); TEST_ASSERT_EQUAL_STRING("PL3", retained.gunLabel);
    const RenderFrame empty;
    const auto cleared = f.display.ut_resolvePaintStatus(context, &empty);
    TEST_ASSERT_FALSE(cleared.hasGunLabel); TEST_ASSERT_FALSE(cleared.alpAlertActive);
}

void test_real_gps_parser_badge_uses_stable_fix_without_speed_eligibility_and_expires() {
    Fixture f;
    const auto paintGpsAt = [&](uint32_t now) {
        mockMillis = now;
        const auto inputs = f.source.captureForPaint();
        const auto context = inputs.context();
        const auto paint = f.display.ut_resolvePaintStatus(context);
        f.display.drawGpsIndicator(paint);
    };
    paintGpsAt(10); TEST_ASSERT_FALSE(f.display.ut_elementCaches().gps.lastShown);
    f.gps.begin(true);
    paintGpsAt(20); TEST_ASSERT_FALSE(f.display.ut_elementCaches().gps.lastShown);
    TEST_ASSERT_TRUE(f.gps.injectNmeaSentenceForTest(
        "$GPGGA,123520,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*4D", 1000));
    paintGpsAt(1000);
    TEST_ASSERT_TRUE(f.display.ut_elementCaches().gps.lastShown);
    TEST_ASSERT_EQUAL_UINT8(8, f.display.ut_elementCaches().gps.lastSats);
    float speed = 0; uint32_t timestamp = 0;
    TEST_ASSERT_FALSE(f.gps.getFreshSpeed(1000, speed, timestamp));
    const auto beforeCapture = f.gps.snapshot(1000);
    f.source.capture(1100);
    const auto afterCapture = f.gps.snapshot(1100);
    TEST_ASSERT_EQUAL_UINT32(beforeCapture.sentencesParsed, afterCapture.sentencesParsed);
    TEST_ASSERT_EQUAL_UINT32(beforeCapture.enableTransitions, afterCapture.enableTransitions);
    TEST_ASSERT_EQUAL_UINT32(beforeCapture.hardwareSamples, afterCapture.hardwareSamples);
    mockMillis = 2000; f.gps.clearSample();
    paintGpsAt(4000); TEST_ASSERT_TRUE(f.display.ut_elementCaches().gps.lastShown);
    f.display.ut_resetDrawnRegion();
    paintGpsAt(4001); TEST_ASSERT_FALSE(f.display.ut_elementCaches().gps.lastShown);
    TEST_ASSERT_FALSE(f.display.ut_drawnRegionEmpty());
    f.display.ut_resetDrawnRegion();
    paintGpsAt(5000); TEST_ASSERT_TRUE(f.display.ut_drawnRegionEmpty());
}

void test_resting_key_observes_resolved_badge_color_when_transport_stays_fresh() {
    Fixture f;
    RenderFrame idle;
    idle.primaryKind = RenderFramePrimaryKind::IDLE;
    idle.primaryState.bogeyCounterChar = idle.primaryState.bogeyCounterChar2 = 'A';
    auto inputs = f.source.captureForPaint();
    inputs.status.ble = {true, true, -70, -65};
    inputs.status.bleProxyEnabled = inputs.status.bleProxyClientConnected = true;
    inputs.status.bleReceiving = true;
    inputs.status.bleSampledAtMs = inputs.status.bleBadgeSampledAtMs = 1000;
    inputs.nowMs = 1500;
    f.display.renderFrame(idle, inputs.context());
    TEST_ASSERT_EQUAL_HEX16(dimColor(f.settings.get().colorBleConnected, 85),
                            f.display.ut_elementCaches().bleProxy.lastColor);
    f.display.testCanvas()->resetCounters();
    // Fresh transport plus an older independently supplied badge observation.
    // The old candidate key sees the same bleFresh/raw flags for this pair.
    inputs.status.bleSampledAtMs = 2000; inputs.nowMs = 2500;
    f.display.renderFrame(idle, inputs.context());
    TEST_ASSERT_EQUAL_HEX16(dimColor(f.settings.get().colorBleConnected, 40),
                            f.display.ut_elementCaches().bleProxy.lastColor);
    TEST_ASSERT_EQUAL_INT(1, f.display.testCanvas()->getFlushCount());
    f.display.testCanvas()->resetCounters();
    f.display.renderFrame(idle, inputs.context());
    TEST_ASSERT_EQUAL_INT(0, f.display.testCanvas()->getFlushCount());
}

void test_resting_key_observes_hysteresis_after_actual_battery_paint() {
    Fixture f;
    RenderFrame idle; idle.primaryKind = RenderFramePrimaryKind::IDLE;
    idle.primaryState.bogeyCounterChar = idle.primaryState.bogeyCounterChar2 = 'A';
    f.batteryAt(4100);
    mockMillis = 100;
    auto inputs = f.source.captureForPaint();
    f.display.renderFrame(idle, inputs.context());
    TEST_ASSERT_TRUE(f.display.ut_elementCaches().battery.lastIconShown);
    // An actual direct status painter changes retained USB hysteresis. Do not
    // assign the private field or erase pending paint bounds to manufacture a skip.
    f.batteryAt(4126); f.paintBattery(100);
    TEST_ASSERT_FALSE(f.display.ut_elementCaches().battery.lastIconShown);
    f.batteryAt(4100);
    const auto after = f.source.captureForPaint();
    const auto context = after.context();
    const auto paint = f.display.ut_resolvePaintStatus(context);
    const auto key = f.display.buildRestingNoOpKey(paint, idle.primaryState);
    TEST_ASSERT_FALSE_MESSAGE(f.display.canSkipRestingNoOp(key),
        "an equal voltage band does not imply equal resolved battery visibility");
    f.display.renderFrame(idle, context);
    TEST_ASSERT_FALSE(f.display.ut_elementCaches().battery.lastIconShown);
}

void test_resting_key_observes_explicit_gun_text_and_lid_color() {
    Fixture f;
    RenderFrame idle; idle.primaryKind = RenderFramePrimaryKind::IDLE;
    idle.primaryState.bogeyCounterChar = idle.primaryState.bogeyCounterChar2 = 'A';
    auto inputs = f.source.captureForPaint(); inputs.nowMs = 100;
    inputs.status.hasGunLabel = true; inputs.status.gunLabelLidActive = true;
    std::strcpy(inputs.status.gunLabel, "PL3");
    f.display.renderFrame(idle, inputs.context());
    TEST_ASSERT_EQUAL_STRING("PL3", f.display.ut_elementCaches().frequency.lastText);
    f.display.testCanvas()->resetCounters();
    std::strcpy(inputs.status.gunLabel, "LTI");
    inputs.status.gunLabelLidActive = false;
    f.display.renderFrame(idle, inputs.context());
    TEST_ASSERT_EQUAL_STRING("LTI", f.display.ut_elementCaches().frequency.lastText);
    TEST_ASSERT_EQUAL_HEX16(f.settings.get().colorAlpDli, f.display.ut_elementCaches().frequency.lastColor);
    TEST_ASSERT_EQUAL_INT(1, f.display.testCanvas()->getFlushCount());
    inputs.status.hasGunLabel = false;
    f.display.renderFrame(idle, inputs.context());
    TEST_ASSERT_EQUAL_STRING("--.---", f.display.ut_elementCaches().frequency.lastText);
}


// Only the canvas boundary records text; warning resolution, screen painting and
// PowerModule ownership below are production code. No shutdown window expires.
class LowBatteryCanvas : public Arduino_Canvas {
  public:
    struct Text {
        std::string value;
        int16_t x, y;
        uint16_t color;
        uint8_t size;
    };
    LowBatteryCanvas() : Arduino_Canvas(SCREEN_WIDTH, SCREEN_HEIGHT, nullptr) {}
    std::vector<Text> text;
    void setTextSize(uint8_t size) override { size_ = size; }
    void setTextColor(uint16_t color) override { color_ = color; }
    void setTextColor(uint16_t color, uint16_t) override { color_ = color; }
    void setCursor(int16_t x, int16_t y) override { x_ = x; y_ = y; }
    void print(const char* value) override { text.push_back({value, x_, y_, color_, size_}); }
    void resetPaint() { resetCounters(); text.clear(); }
  private:
    int16_t x_ = 0, y_ = 0;
    uint16_t color_ = 0;
    uint8_t size_ = 0;
};

void test_zero_volume_warning_real_painter_flashes_and_restores_idle_frequency() {
    Fixture f;
    auto* canvas = new LowBatteryCanvas;
    f.display.setTestCanvas(canvas);
    f.display.ut_fontMgr().segment7Ready = true;
    RenderFrame idle;
    idle.primaryKind = RenderFramePrimaryKind::IDLE;
    idle.primaryState.bogeyCounterChar = idle.primaryState.bogeyCounterChar2 = 'L';
    idle.primaryState.bogeyCounterByte = idle.primaryState.bogeyCounterByte2 = 0x38;
    idle.primaryState.hasVolumeData = true;
    idle.primaryState.mainVolume = 0;
    const auto paintAt = [&](uint32_t now) {
        mockMillis = now;
        f.source.noteBleSample({true, false, -65, 0}, false, now);
        const auto inputs = f.source.captureForPaint();
        f.display.renderFrame(idle, inputs.context());
    };
    const auto warningTextCount = [&]() {
        unsigned count = 0;
        for (const auto& text : canvas->text) {
            if (text.value != "VOL 0") continue;
            ++count;
            TEST_ASSERT_EQUAL_INT(190, text.x);
            TEST_ASSERT_EQUAL_INT(45, text.y);
            TEST_ASSERT_EQUAL_HEX16(0xF800, text.color);
            TEST_ASSERT_EQUAL_UINT8(6, text.size);
        }
        return count;
    };

    paintAt(1000);
    TEST_ASSERT_EQUAL_STRING("--.---", f.display.ut_elementCaches().frequency.lastText);
    canvas->resetPaint();
    paintAt(17000); // First eligible frame starts the existing blank phase.
    TEST_ASSERT_EQUAL_UINT(0, warningTextCount());
    TEST_ASSERT_EQUAL_INT(1, canvas->getFlushCount());
    TEST_ASSERT_FALSE(f.display.volumeZeroWarningRefreshDue(17249));
    TEST_ASSERT_TRUE(f.display.volumeZeroWarningRefreshDue(17250));
    canvas->resetPaint();
    paintAt(17250);
    TEST_ASSERT_EQUAL_UINT(1, warningTextCount());
    TEST_ASSERT_EQUAL_INT(1, canvas->getFlushCount());
    TEST_ASSERT_FALSE(f.display.volumeZeroWarningRefreshDue(17499));
    TEST_ASSERT_TRUE(f.display.volumeZeroWarningRefreshDue(17500));
    canvas->resetPaint();
    paintAt(17500);
    TEST_ASSERT_EQUAL_UINT(0, warningTextCount());
    TEST_ASSERT_EQUAL_INT(1, canvas->getFlushCount());
    canvas->resetPaint();
    paintAt(27000);
    TEST_ASSERT_EQUAL_STRING("--.---", f.display.ut_elementCaches().frequency.lastText);
    TEST_ASSERT_EQUAL_UINT(0, warningTextCount());
    TEST_ASSERT_EQUAL_INT(1, canvas->getFlushCount());
    TEST_ASSERT_FALSE(f.display.volumeZeroWarningRefreshDue(27250));
    canvas->resetPaint();
    paintAt(27250);
    TEST_ASSERT_EQUAL_INT(0, canvas->getFlushCount());
}

void test_power_warning_uses_fresh_inputs_through_preemption_restore_and_release() {
    Fixture f;
    auto* canvas = new LowBatteryCanvas;
    f.display.setTestCanvas(canvas);
    f.batteryAt(3200, 4);
    f.battery.setOnBattery(true);
    f.settings.settings.autoPowerOffMinutes = 0;
    f.source.noteBleSample({true, true, -65, -70}, true, 50);
    PowerModule power;
    power.begin(&f.battery, &f.display, &f.settings, f.source);

    const auto expectWarning = [&]() {
        // Literal geometry/text for the existing 640x172 painter. Text position
        // reflects the existing canvas substitute's 8px-wide / 10px-high bounds.
        TEST_ASSERT_EQUAL_INT(1, canvas->getFillScreenCount());
        TEST_ASSERT_EQUAL_HEX16(0x0000, canvas->getLastFillColor());
        TEST_ASSERT_EQUAL_UINT32(1, canvas->drawRectCalls.size());
        const auto& outline = canvas->drawRectCalls[0];
        TEST_ASSERT_EQUAL_INT(260, outline.x); TEST_ASSERT_EQUAL_INT(36, outline.y);
        TEST_ASSERT_EQUAL_INT(120, outline.w); TEST_ASSERT_EQUAL_INT(60, outline.h);
        TEST_ASSERT_EQUAL_HEX16(0xf800, outline.color);
        TEST_ASSERT_EQUAL_UINT32(2, canvas->fillRectCalls.size());
        const auto& cap = canvas->fillRectCalls[0];
        TEST_ASSERT_EQUAL_INT(380, cap.x); TEST_ASSERT_EQUAL_INT(54, cap.y);
        TEST_ASSERT_EQUAL_INT(12, cap.w); TEST_ASSERT_EQUAL_INT(24, cap.h);
        TEST_ASSERT_EQUAL_HEX16(0xf800, cap.color);
        const auto& bar = canvas->fillRectCalls[1];
        TEST_ASSERT_EQUAL_INT(268, bar.x); TEST_ASSERT_EQUAL_INT(44, bar.y);
        TEST_ASSERT_EQUAL_INT(20, bar.w); TEST_ASSERT_EQUAL_INT(44, bar.h);
        TEST_ASSERT_EQUAL_HEX16(0xf800, bar.color);
        TEST_ASSERT_EQUAL_UINT32(1, canvas->text.size());
        TEST_ASSERT_EQUAL_STRING("LOW BATTERY", canvas->text[0].value.c_str());
        TEST_ASSERT_EQUAL_INT(276, canvas->text[0].x); TEST_ASSERT_EQUAL_INT(121, canvas->text[0].y);
        TEST_ASSERT_EQUAL_HEX16(0xf800, canvas->text[0].color);
        TEST_ASSERT_EQUAL_UINT8(2, canvas->text[0].size);
        TEST_ASSERT_EQUAL_INT(1, canvas->getFlushCount());
    };

    mockMillis = 100;
    power.process();
    TEST_ASSERT_TRUE(power.ownsDisplayPresentation());
    TEST_ASSERT_FALSE(power.consumeDisplayRestoreRequest());
    TEST_ASSERT_EQUAL_INT(0, canvas->getFlushCount());
    TEST_ASSERT_EQUAL_UINT32(0, obdReads);
    TEST_ASSERT_EQUAL_INT(1, f.battery.updateCalls);

    mockMillis = 101;
    power.restoreCriticalBatteryWarning();
    expectWarning();
    TEST_ASSERT_EQUAL_UINT32(101, obdReadAt);
    TEST_ASSERT_TRUE(f.display.lastProxyClientConnected_); // Actual resolver consumed the capture.
    power.restoreCriticalBatteryWarning();
    TEST_ASSERT_EQUAL_INT(1, canvas->getFlushCount());
    TEST_ASSERT_EQUAL_UINT32(1, obdReads);
    TEST_ASSERT_EQUAL_INT(1, f.battery.updateCalls); // Capture/painting do not poll the battery.

    // The existing runtime arbitration calls this boundary when a live frame
    // takes the pixels. It does not release the armed critical-battery owner.
    canvas->resetPaint();
    power.noteCriticalBatteryWarningPreempted();
    RenderFrame live;
    live.primaryKind = RenderFramePrimaryKind::V1_LIVE;
    live.v1Priority = AlertData::create(BAND_K, DIR_FRONT, 4, 0, 24150, true, true);
    live.primaryState.activeBands = BAND_K;
    live.primaryState.arrows = live.primaryState.priorityArrow = DIR_FRONT;
    live.primaryState.signalBars = 4;
    live.primaryState.bogeyCounterChar = live.primaryState.bogeyCounterChar2 = '1';
    mockMillis = 1000;
    const auto liveInputs = f.source.captureForPaint();
    f.display.renderFrame(live, liveInputs.context());
    TEST_ASSERT_TRUE(power.ownsDisplayPresentation());
    TEST_ASSERT_TRUE(power.criticalBatteryWarningNeedsRestore());
    TEST_ASSERT_EQUAL_INT(BAND_K, f.display.ut_elementCaches().bands.lastMask);
    TEST_ASSERT_TRUE(f.display.ut_elementCaches().arrow.showFront);
    TEST_ASSERT_EQUAL_STRING("24.150", f.display.ut_elementCaches().frequency.lastText);
    TEST_ASSERT_EQUAL_INT(1, canvas->getFlushCount());

    canvas->resetPaint();
    f.source.noteBleSample({true, false, -80, 0}, true, 1100);
    mockMillis = 1350;
    power.restoreCriticalBatteryWarning();
    expectWarning();
    TEST_ASSERT_TRUE(power.ownsDisplayPresentation());
    TEST_ASSERT_FALSE(power.criticalBatteryWarningNeedsRestore());
    TEST_ASSERT_EQUAL_UINT32(1350, obdReadAt);
    TEST_ASSERT_FALSE(f.display.lastProxyClientConnected_);
    power.restoreCriticalBatteryWarning();
    TEST_ASSERT_EQUAL_INT(1, canvas->getFlushCount());
    TEST_ASSERT_EQUAL_UINT32(3, obdReads);
    TEST_ASSERT_EQUAL_INT(1, f.battery.updateCalls);
    TEST_ASSERT_EQUAL_INT(0, f.battery.refreshVoltageCalls);
    TEST_ASSERT_EQUAL_INT(0, f.battery.powerOffCalls);

    canvas->resetPaint();
    f.batteryAt(3900, 80);
    mockMillis = 2000;
    power.process();
    TEST_ASSERT_FALSE(power.ownsDisplayPresentation());
    TEST_ASSERT_FALSE(power.criticalBatteryWarningNeedsRestore());
    TEST_ASSERT_TRUE(power.consumeDisplayRestoreRequest());
    TEST_ASSERT_FALSE(power.consumeDisplayRestoreRequest());
    power.restoreCriticalBatteryWarning();
    TEST_ASSERT_EQUAL_INT(0, canvas->getFlushCount());
    TEST_ASSERT_EQUAL_UINT32(3, obdReads);

    // Fulfill the one authoritative-restore request with current resting input,
    // using the same existing invalidation boundary as the runtime caller.
    f.display.forceNextRedraw();
    RenderFrame idle; idle.primaryKind = RenderFramePrimaryKind::IDLE;
    idle.primaryState.bogeyCounterChar = idle.primaryState.bogeyCounterChar2 = 'A';
    const auto currentInputs = f.source.captureForPaint();
    f.display.renderFrame(idle, currentInputs.context());
    TEST_ASSERT_EQUAL_INT(1, canvas->getFlushCount());
    TEST_ASSERT_EQUAL_INT(BAND_NONE, f.display.ut_elementCaches().bands.lastMask);
    TEST_ASSERT_FALSE(f.display.ut_elementCaches().arrow.showFront);
    TEST_ASSERT_EQUAL_STRING("--.---", f.display.ut_elementCaches().frequency.lastText);
    for (const auto& text : canvas->text) TEST_ASSERT_NOT_EQUAL(0, text.value.compare("LOW BATTERY"));
    TEST_ASSERT_EQUAL_UINT32(2000, obdReadAt);
    TEST_ASSERT_EQUAL_UINT32(4, obdReads);
    TEST_ASSERT_EQUAL_INT(2, f.battery.updateCalls);
    TEST_ASSERT_EQUAL_INT(0, f.battery.refreshVoltageCalls);
    TEST_ASSERT_EQUAL_INT(0, f.battery.powerOffCalls);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_resting_key_observes_resolved_badge_color_when_transport_stays_fresh);
    RUN_TEST(test_resting_key_observes_hysteresis_after_actual_battery_paint);
    RUN_TEST(test_resting_key_observes_explicit_gun_text_and_lid_color);
    RUN_TEST(test_real_gps_parser_badge_uses_stable_fix_without_speed_eligibility_and_expires);
    RUN_TEST(test_unbound_source_is_unavailable_and_preserves_original_ble_age);
    RUN_TEST(test_capture_reads_each_observation_once_without_refreshing_or_starting_producers);
    RUN_TEST(test_resolution_matches_literal_gps_obd_wifi_and_ble_boundaries);
    RUN_TEST(test_profile_selection_is_paint_free_and_delayed_paint_does_not_extend_reveal);
    RUN_TEST(test_profile_selection_zero_wrap_and_preview_restore_use_one_timer);
    RUN_TEST(test_battery_hysteresis_commits_only_when_painted_and_uses_exact_thresholds);
    RUN_TEST(test_battery_percent_cadence_and_profile_overlap_share_resolved_values);
    RUN_TEST(test_status_only_paint_preserves_same_session_gun_but_new_empty_frame_clears_it);
    RUN_TEST(test_power_warning_uses_fresh_inputs_through_preemption_restore_and_release);
    RUN_TEST(test_zero_volume_warning_real_painter_flashes_and_restores_idle_frequency);
    return UNITY_END();
}
