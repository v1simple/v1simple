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
#include "../../src/packet_parser.h"
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
    struct TextCall { std::string text; uint16_t color; };
    struct FlushSnapshot {
        std::vector<FillRectCall> rectangles;
        std::vector<FillTriangleCall> triangles;
        std::vector<TextCall> text;
    };
    std::vector<std::string> printed;
    std::vector<TextCall> textCalls;
    std::vector<FlushSnapshot> flushSnapshots;
    uint16_t textColor = TFT_WHITE;
    void setTextColor(uint16_t color) override { textColor = color; }
    void setTextColor(uint16_t color, uint16_t) override { textColor = color; }
    void print(const char* text) override {
        printed.emplace_back(text);
        textCalls.push_back({text, textColor});
    }
    void flush() override {
        // These are actual source paint requests at dispatch time. The mock
        // does not rasterize glyphs/triangles or model panel scan and response.
        flushSnapshots.push_back({fillRectCalls, fillTriangleCalls, textCalls});
        Arduino_Canvas::flush();
    }
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
static int stealthDrawCalls = 0;
void V1Display::showStealth(float, bool) {
    ++stealthDrawCalls;
    currentScreen_ = ScreenMode::Stealth;
}
void V1Display::showScanning() {}
void V1Display::setBleContext(const DisplayBleContext& context) { bleCtx_ = context; }
void V1Display::setBLEProxyStatus(bool, bool, bool) {}
void V1Display::setSpeedVolZeroActive(bool active) { speedVolZeroActive_ = active; }
void V1Display::setAlpLaserEvent(const AlpLaserEvent&) {}
void V1Display::forceNextRedraw() { dirty_.resetTracking = true; }
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
#include "../../src/packet_parser.cpp"
#include "../../src/packet_parser_alerts.cpp"
#include "../../include/display_mode.h"
#include "../mocks/ble_client.h"
#include "../../src/modules/alert_persistence/alert_persistence_module.h"
#include "../../src/modules/speed/speed_source_selector.h"
#include "../../src/modules/speed_mute/speed_mute_module.h"
#include "../../src/modules/volume_fade/volume_fade_module.h"
#include "../../src/modules/display/display_preview_module.h"
#include "../../src/modules/display/display_restore_module.h"
#include "../../src/modules/display/display_orchestration_module.cpp"

// Preview/restore lifecycle is inert except for the ownership flag under test.
DisplayPreviewModule::DisplayPreviewModule() = default;
void DisplayPreviewModule::update() {}
void DisplayPreviewModule::requestHold(uint32_t) { previewActive_ = true; }
void DisplayPreviewModule::cancel() { previewActive_ = false; }
bool DisplayRestoreModule::process() { return false; }

V1Display display(settings);

namespace {
RecordingCanvas* canvas() { return static_cast<RecordingCanvas*>(display.testCanvas()); }

void clearObservations() {
    canvas()->resetCounters();
    canvas()->printed.clear();
    canvas()->textCalls.clear();
    canvas()->flushSnapshots.clear();
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

void assertPriorityArrowPresentation(bool priorityOnly, Direction transmittedArrows,
                                     Direction priorityArrow, Direction expectedArrows) {
    // Select a nondefault slot with an opposing inactive-slot policy. The main
    // display must follow the user's active slot while retaining the rear X card.
    settings.mutableSettings().activeSlot = 1;
    settings.slotPriorityArrowOnly[0] = !priorityOnly;
    settings.slotPriorityArrowOnly[1] = priorityOnly;
    RenderFrame frame;
    frame.primaryKind = RenderFramePrimaryKind::V1_LIVE;
    frame.v1Priority = AlertData::create(BAND_K, DIR_FRONT, 4, 0, 24150, true, true);
    frame.primaryState = stateFor(frame.v1Priority, '2', 4);
    frame.primaryState.arrows = transmittedArrows;
    frame.primaryState.priorityArrow = priorityArrow;
    frame.cardCount = 1;
    frame.cards[0].kind = RenderFrameCard::Kind::V1;
    frame.cards[0].v1Alert = AlertData::create(BAND_X, DIR_REAR, 0, 2, 10525, true, false);
    display.renderFrame(frame);

    TEST_ASSERT_EQUAL_UINT(1, canvas()->flushSnapshots.size());
    TEST_ASSERT_TRUE(regionalTransfers.empty());
    const auto& sent = canvas()->flushSnapshots.front();
    // The main cluster is painted before the card's own direction symbol.
    TEST_ASSERT_TRUE(sent.triangles.size() >= 4);
    TEST_ASSERT_TRUE(sent.triangles[0].y0 < sent.triangles[0].y1);
    TEST_ASSERT_TRUE(sent.triangles[1].x0 < sent.triangles[1].x1);
    TEST_ASSERT_TRUE(sent.triangles[2].x0 > sent.triangles[2].x1);
    TEST_ASSERT_TRUE(sent.triangles[3].y0 > sent.triangles[3].y1);
    TEST_ASSERT_EQUAL_HEX16((expectedArrows & DIR_FRONT) ? settings.get().colorArrowFront : TFT_DARKGREY,
                           sent.triangles[0].color);
    TEST_ASSERT_EQUAL_HEX16((expectedArrows & DIR_SIDE) ? settings.get().colorArrowSide : TFT_DARKGREY,
                           sent.triangles[1].color);
    TEST_ASSERT_EQUAL_HEX16((expectedArrows & DIR_SIDE) ? settings.get().colorArrowSide : TFT_DARKGREY,
                           sent.triangles[2].color);
    TEST_ASSERT_EQUAL_HEX16((expectedArrows & DIR_REAR) ? settings.get().colorArrowRear : TFT_DARKGREY,
                           sent.triangles[3].color);
    TEST_ASSERT_EQUAL_INT(1, display.ut_elementCaches().cards.lastDrawnCount);
    const auto& card = display.ut_elementCaches().cards.lastDrawnPositions[0];
    TEST_ASSERT_EQUAL_INT(BAND_X, card.band);
    TEST_ASSERT_EQUAL_UINT32(10525, card.frequency);
    TEST_ASSERT_EQUAL_INT(DIR_REAR, card.direction);
    TEST_ASSERT_FALSE(card.isGraced);
    TEST_ASSERT_TRUE(std::find(canvas()->printed.begin(), canvas()->printed.end(), "10.525") != canvas()->printed.end());
}
} // namespace

void setUp() {
    mockMillis = mockMicros = 1000;
    settings = SettingsManager{};
    stealthDrawCalls = 0;
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

// Ordinary replay transitions that briefly showed FRONT+SIDE and Ka+X in
// recorded acquisition images. They have no configured alert persistence.
// The claim here stops at the complete source paint/flush boundary; it does
// not attribute mixed optical observations to firmware, panel or camera.
void test_front_to_side_replaces_outgoing_active_paint_before_full_flush() {
    settings.slotAlertPersistSec[0] = 0;
    const auto front = AlertData::create(BAND_K, DIR_FRONT, 3, 0, 24150, true, true);
    const auto side = AlertData::create(BAND_K, DIR_SIDE, 3, 0, 24150, true, true);
    showLive(front, nullptr, 4);
    clearObservations();
    showLive(side, nullptr, 4);

    TEST_ASSERT_EQUAL_UINT(1, canvas()->flushSnapshots.size());
    TEST_ASSERT_EQUAL_INT(1, canvas()->getFlushCount());
    TEST_ASSERT_TRUE(regionalTransfers.empty());
    const auto& sent = canvas()->flushSnapshots.front();
    // Front triangle, left/right side heads, rear triangle. Assert geometry as
    // well as order so a missing or extra direction cannot satisfy the colors.
    TEST_ASSERT_EQUAL_UINT(4, sent.triangles.size());
    const auto& frontPaint = sent.triangles[0];
    const auto& leftPaint = sent.triangles[1];
    const auto& rightPaint = sent.triangles[2];
    const auto& rearPaint = sent.triangles[3];
    TEST_ASSERT_TRUE(frontPaint.y0 < frontPaint.y1 && frontPaint.y1 == frontPaint.y2);
    TEST_ASSERT_TRUE(leftPaint.x0 < leftPaint.x1 && leftPaint.x1 == leftPaint.x2);
    TEST_ASSERT_TRUE(rightPaint.x0 > rightPaint.x1 && rightPaint.x1 == rightPaint.x2);
    TEST_ASSERT_TRUE(rearPaint.y0 > rearPaint.y1 && rearPaint.y1 == rearPaint.y2);
    TEST_ASSERT_EQUAL_HEX16(TFT_DARKGREY, frontPaint.color);
    TEST_ASSERT_EQUAL_HEX16(settings.get().colorArrowSide, leftPaint.color);
    TEST_ASSERT_EQUAL_HEX16(settings.get().colorArrowSide, rightPaint.color);
    TEST_ASSERT_EQUAL_HEX16(TFT_DARKGREY, rearPaint.color);
    const bool fullClusterCleared = std::any_of(sent.rectangles.begin(), sent.rectangles.end(),
        [&](const Arduino_Canvas::FillRectCall& r) {
            return r.color == TFT_BLACK &&
                   r.x <= frontPaint.x1 && r.x + r.w > frontPaint.x2 &&
                   r.y <= frontPaint.y0 && r.y + r.h > rearPaint.y0;
        });
    TEST_ASSERT_TRUE_MESSAGE(fullClusterCleared, "the prior active cluster must be cleared before dispatch");
    TEST_ASSERT_FALSE(display.ut_elementCaches().arrow.showFront);
    TEST_ASSERT_TRUE(display.ut_elementCaches().arrow.showSide);
    TEST_ASSERT_FALSE(display.ut_elementCaches().arrow.showRear);

    clearObservations();
    showLive(side, nullptr, 4);
    TEST_ASSERT_TRUE(canvas()->fillTriangleCalls.empty());
    TEST_ASSERT_TRUE(canvas()->flushSnapshots.empty());
    TEST_ASSERT_TRUE(regionalTransfers.empty());
}

void test_ka_to_x_replaces_outgoing_active_band_before_full_flush() {
    settings.slotAlertPersistSec[0] = 0;
    const auto ka = AlertData::create(BAND_KA, DIR_SIDE, 4, 0, 34700, true, true);
    const auto x = AlertData::create(BAND_X, DIR_FRONT, 2, 0, 10525, true, true);
    showLive(ka, nullptr, 5);
    clearObservations();
    showLive(x, nullptr, 2);

    TEST_ASSERT_EQUAL_UINT(1, canvas()->flushSnapshots.size());
    TEST_ASSERT_EQUAL_INT(1, canvas()->getFlushCount());
    TEST_ASSERT_TRUE(regionalTransfers.empty());
    const auto& sent = canvas()->flushSnapshots.front();
    unsigned kaPaints = 0;
    unsigned xPaints = 0;
    for (const auto& call : sent.text) {
        if (call.text == "Ka") {
            ++kaPaints;
            TEST_ASSERT_EQUAL_HEX16(TFT_DARKGREY, call.color);
        }
        if (call.text == "X") {
            ++xPaints;
            TEST_ASSERT_EQUAL_HEX16(settings.get().colorBandX, call.color);
        }
    }
    TEST_ASSERT_EQUAL_UINT(1, kaPaints);
    TEST_ASSERT_EQUAL_UINT(1, xPaints);
    TEST_ASSERT_EQUAL_INT(BAND_X, display.ut_elementCaches().bands.lastMask);
    TEST_ASSERT_EQUAL_INT(0, display.ut_elementCaches().cards.lastDrawnCount);

    clearObservations();
    showLive(x, nullptr, 2);
    TEST_ASSERT_TRUE(canvas()->textCalls.empty());
    TEST_ASSERT_TRUE(canvas()->flushSnapshots.empty());
    TEST_ASSERT_TRUE(regionalTransfers.empty());
}

void test_priority_arrow_disabled_keeps_all_transmitted_directions() {
    const auto allDirections = static_cast<Direction>(DIR_FRONT | DIR_SIDE | DIR_REAR);
    assertPriorityArrowPresentation(false, allDirections, DIR_FRONT, allDirections);
}

void test_priority_arrow_enabled_filters_main_directions_but_keeps_secondary() {
    assertPriorityArrowPresentation(true, static_cast<Direction>(DIR_FRONT | DIR_SIDE | DIR_REAR),
                                    DIR_FRONT, DIR_FRONT);
}

void test_priority_arrow_enabled_does_not_invent_untransmitted_priority_direction() {
    assertPriorityArrowPresentation(true, static_cast<Direction>(DIR_SIDE | DIR_REAR),
                                    DIR_FRONT, DIR_NONE);
}

void test_priority_arrow_enabled_with_no_priority_direction_keeps_main_arrows_inactive() {
    assertPriorityArrowPresentation(true, static_cast<Direction>(DIR_FRONT | DIR_SIDE | DIR_REAR),
                                    DIR_NONE, DIR_NONE);
}

namespace {
struct CounterBlinkRuntime {
    PacketParser parser;
    V1BLEClient ble;
    AlertPersistenceModule persistence;
    VoiceModule voice;
    AlpRuntimeModule alp;
    SpeedSourceSelector speed;
    DisplayMode mode = DisplayMode::IDLE;
    DisplayPipelineModule pipeline;
    DisplayPreviewModule preview;
    DisplayOrchestrationModule orchestration;

    CounterBlinkRuntime() {
        settings.slotAlertPersistSec[0] = 0;
        DisplayPipelineDependencies dependencies;
        dependencies.displayMode = &mode;
        dependencies.display = &display;
        dependencies.parser = &parser;
        dependencies.settings = &settings;
        dependencies.ble = &ble;
        dependencies.alertPersistence = &persistence;
        dependencies.voice = &voice;
        dependencies.alp = &alp;
        dependencies.speedSelector = &speed;
        pipeline.begin(dependencies);
        orchestration.begin(&display, &ble, nullptr, &preview, nullptr, &parser, &settings,
                            nullptr, nullptr, nullptr, &pipeline);
    }

    void feed(uint8_t id, std::initializer_list<uint8_t> payload, uint32_t nowMs = 10000) {
        std::vector<uint8_t> packet{0xAA, 0xD6, 0xEA, id, static_cast<uint8_t>(payload.size() + 1)};
        packet.insert(packet.end(), payload);
        uint8_t checksum = 0;
        for (uint8_t value : packet) checksum = static_cast<uint8_t>(checksum + value);
        packet.push_back(checksum);
        packet.push_back(0xAB);
        mockMillis = nowMs;
        TEST_ASSERT_TRUE(parser.parse(packet.data(), packet.size(), nowMs));
    }

    void counter(uint8_t on, uint8_t off, uint8_t bandArrows = 0) {
        // Complete InfDisplayData packet; aux0 keeps system-status/display-on
        // set, and aux2 supplies the known main/mute volume pair 6/2.
        feed(PACKET_ID_DISPLAY_DATA, {on, off, 0, bandArrows, bandArrows, 0x0C, 0, 0x62});
        pipeline.handleParsed(static_cast<uint32_t>(mockMillis));
    }

    bool refresh(uint32_t nowMs, DisplayOrchestrationRefreshContext context = {}) {
        mockMillis = nowMs;
        context.nowMs = nowMs;
        const bool requested = orchestration.processLightweightRefresh(context).runBlinkRefresh;
        if (requested) pipeline.refreshBlinkTick(nowMs);
        return requested;
    }
};

void assertCounterBlinkCycle(CounterBlinkRuntime& runtime, const char* on) {
    TEST_ASSERT_EQUAL_STRING(on, display.ut_elementCaches().topCounter.lastText);
    TEST_ASSERT_EQUAL_STRING(on, display.ut_fontMgr().segment7.lastPrinted);
    TEST_ASSERT_EQUAL_INT(1, canvas()->getFlushCount());
    clearObservations();
    TEST_ASSERT_FALSE(runtime.refresh(10095));
    TEST_ASSERT_TRUE(canvas()->flushSnapshots.empty());
    TEST_ASSERT_TRUE(runtime.refresh(10096));
    TEST_ASSERT_EQUAL_STRING(" ", display.ut_elementCaches().topCounter.lastText);
    TEST_ASSERT_EQUAL_STRING(" ", display.ut_fontMgr().segment7.lastPrinted);
    TEST_ASSERT_EQUAL_UINT(1, canvas()->flushSnapshots.size());
    TEST_ASSERT_FALSE(canvas()->flushSnapshots[0].rectangles.empty());
    TEST_ASSERT_TRUE(regionalTransfers.empty());
    clearObservations();
    TEST_ASSERT_FALSE(runtime.refresh(10096));
    TEST_ASSERT_TRUE(canvas()->flushSnapshots.empty());
    TEST_ASSERT_TRUE(runtime.refresh(10192));
    TEST_ASSERT_EQUAL_STRING(on, display.ut_fontMgr().segment7.lastPrinted);
    TEST_ASSERT_EQUAL_UINT(1, canvas()->flushSnapshots.size());
}
} // namespace

void test_idle_junk_counter_blinks_through_parser_pipeline_and_transfer() {
    CounterBlinkRuntime runtime;
    runtime.counter(0x1E, 0); // J / blank; no radar rows exist.
    TEST_ASSERT_FALSE(runtime.parser.hasAlerts());
    assertCounterBlinkCycle(runtime, "J");
}

void test_idle_steady_counter_does_not_request_extra_transfers() {
    CounterBlinkRuntime runtime;
    runtime.counter(0x06, 0x06); // Steady 1, both images agree.
    clearObservations();
    TEST_ASSERT_FALSE(runtime.refresh(10096));
    TEST_ASSERT_FALSE(runtime.refresh(10192));
    TEST_ASSERT_EQUAL_STRING("1", display.ut_elementCaches().topCounter.lastText);
    TEST_ASSERT_TRUE(canvas()->flushSnapshots.empty());
}

void test_live_counter_keeps_existing_blink_cadence() {
    CounterBlinkRuntime runtime;
    runtime.feed(PACKET_ID_ALERT_DATA, {0x11, 0x87, 0x8C, 0xAC, 0, 0x22, 0x80});
    runtime.counter(0x06, 0, 0x22);
    TEST_ASSERT_TRUE(runtime.parser.hasAlerts());
    assertCounterBlinkCycle(runtime, "1");
}

void test_idle_counter_blink_respects_splash_preview_and_runtime_gates() {
    CounterBlinkRuntime runtime;
    runtime.counter(0x1E, 0);
    clearObservations();
    DisplayOrchestrationRefreshContext context;
    context.bootSplashHoldActive = true;
    TEST_ASSERT_FALSE(runtime.refresh(10096, context));
    context = {};
    context.pipelineRanThisLoop = true;
    TEST_ASSERT_FALSE(runtime.refresh(10096, context));
    context = {};
    context.overloadLateThisLoop = true;
    TEST_ASSERT_FALSE(runtime.refresh(10096, context));
    runtime.preview.requestHold(1000);
    TEST_ASSERT_FALSE(runtime.refresh(10096));
    runtime.preview.cancel();
    runtime.ble.setConnected(false);
    TEST_ASSERT_FALSE(runtime.refresh(10096));
    TEST_ASSERT_TRUE(canvas()->flushSnapshots.empty());
    TEST_ASSERT_EQUAL_STRING("J", display.ut_elementCaches().topCounter.lastText);
    runtime.ble.setConnected(true);
    TEST_ASSERT_TRUE(runtime.refresh(10096));
    TEST_ASSERT_EQUAL_STRING(" ", display.ut_elementCaches().topCounter.lastText);
    TEST_ASSERT_EQUAL_UINT(1, canvas()->flushSnapshots.size());
}

void test_idle_blink_refresh_does_not_replace_stealth_owner() {
    CounterBlinkRuntime runtime;
    settings.mutableSettings().stealthEnabled = true;
    runtime.counter(0x1E, 0);
    TEST_ASSERT_EQUAL_INT(1, stealthDrawCalls);
    clearObservations();
    TEST_ASSERT_FALSE(runtime.refresh(10096));
    TEST_ASSERT_FALSE(runtime.refresh(10097));
    TEST_ASSERT_FALSE(runtime.refresh(10192));
    runtime.pipeline.refreshBlinkTick(10192); // Defensive direct entry also preserves ownership.
    TEST_ASSERT_EQUAL_INT(1, stealthDrawCalls);
    TEST_ASSERT_TRUE(canvas()->flushSnapshots.empty());
    TEST_ASSERT_EQUAL_STRING("", display.ut_elementCaches().topCounter.lastText);
}

void test_live_v1_counter_blinks_after_taking_stealth_screen() {
    CounterBlinkRuntime runtime;
    settings.mutableSettings().stealthEnabled = true;
    runtime.counter(0x1E, 0);
    TEST_ASSERT_TRUE(display.isStealthScreen());
    runtime.feed(PACKET_ID_ALERT_DATA, {0x11, 0x87, 0x8C, 0xAC, 0, 0x22, 0x80});
    runtime.counter(0x06, 0, 0x22);
    TEST_ASSERT_FALSE(display.isStealthScreen());
    assertCounterBlinkCycle(runtime, "1");
}

void test_live_alp_counter_blinks_after_taking_stealth_screen() {
    CounterBlinkRuntime runtime;
    settings.mutableSettings().stealthEnabled = true;
    runtime.counter(0x1E, 0);
    TEST_ASSERT_TRUE(display.isStealthScreen());
    runtime.alp.testSetEnabled(true);
    runtime.alp.testSetState(AlpState::ALERT_ACTIVE);
    runtime.alp.testOpenSession(AlpGunType::MARKSMAN_ULTRALYTE, false, AlpLaserDirection::FRONT);
    runtime.counter(0x1E, 0);
    TEST_ASSERT_FALSE(runtime.parser.hasAlerts());
    TEST_ASSERT_FALSE(display.isStealthScreen());
    assertCounterBlinkCycle(runtime, "J");
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_event0007_new_x_with_unchanged_ka_primary_paints_and_full_flushes);
    RUN_TEST(test_event0010_new_x_with_unchanged_k_primary_paints_and_full_flushes);
    RUN_TEST(test_live_counter_only_change_full_flushes_then_cache_hit_skips);
    RUN_TEST(test_live_pending_draw_full_flushes_even_when_frame_itself_is_unchanged);
    RUN_TEST(test_front_to_side_replaces_outgoing_active_paint_before_full_flush);
    RUN_TEST(test_ka_to_x_replaces_outgoing_active_band_before_full_flush);
    RUN_TEST(test_priority_arrow_disabled_keeps_all_transmitted_directions);
    RUN_TEST(test_priority_arrow_enabled_filters_main_directions_but_keeps_secondary);
    RUN_TEST(test_priority_arrow_enabled_does_not_invent_untransmitted_priority_direction);
    RUN_TEST(test_priority_arrow_enabled_with_no_priority_direction_keeps_main_arrows_inactive);
    RUN_TEST(test_idle_junk_counter_blinks_through_parser_pipeline_and_transfer);
    RUN_TEST(test_idle_steady_counter_does_not_request_extra_transfers);
    RUN_TEST(test_live_counter_keeps_existing_blink_cadence);
    RUN_TEST(test_idle_counter_blink_respects_splash_preview_and_runtime_gates);
    RUN_TEST(test_idle_blink_refresh_does_not_replace_stealth_owner);
    RUN_TEST(test_live_v1_counter_blinks_after_taking_stealth_screen);
    RUN_TEST(test_live_alp_counter_blinks_after_taking_stealth_screen);
    return UNITY_END();
}
