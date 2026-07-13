#include <unity.h>

#include <new>

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
#include "../../src/perf_metrics.h"

V1Display* g_displayInstance = nullptr;
SettingsManager settingsManager;
PerfCounters perfCounters;
PerfExtendedMetrics perfExtended;

V1Display::V1Display() {
    currentPalette_ = ColorThemes::STANDARD();
    currentPalette_.colorMuted = settingsManager.get().colorMuted;
    currentPalette_.colorPersisted = settingsManager.get().colorPersisted;
    g_displayInstance = this;
}

V1Display::~V1Display() = default;

PerfDisplayScreen V1Display::perfScreenForMode(ScreenMode mode) {
    switch (mode) {
        case ScreenMode::Resting:
            return PerfDisplayScreen::Resting;
        case ScreenMode::Scanning:
            return PerfDisplayScreen::Scanning;
        case ScreenMode::Live:
            return PerfDisplayScreen::Live;
        case ScreenMode::Persisted:
            return PerfDisplayScreen::Persisted;
        case ScreenMode::Stealth:
            return PerfDisplayScreen::Resting;
        case ScreenMode::Disconnected:
        case ScreenMode::Unknown:
        default:
            return PerfDisplayScreen::Unknown;
    }
}

// Controls let each test simulate which leaf draws annotate drawnRegion_.
// Real leaf draws annotate after cache early-returns; the test mimics that by
// toggling globals that the stubs consult. See partial-flush region-union plan:
// docs/plans/PARTIAL_FLUSH_REGION_UNION_20260422.md.
namespace {
    bool g_stubArrowAnnotates  = false;
    bool g_stubFreqAnnotates   = false;
    bool g_stubBandsAnnotates  = false;  // annotates kBandsColumnRect
    bool g_stubWifiAnnotates   = false;  // annotates a bottom-left status icon
    bool g_stubBigRectAnnotate = false;  // forces union over cap
    bool g_stubArrowVisibilityChanged = false;
    int  g_flushRegionCalls    = 0;
    int16_t g_lastFlushX = -1;
    int16_t g_lastFlushY = -1;
    int16_t g_lastFlushW = -1;
    int16_t g_lastFlushH = -1;
    int  g_partialRegionFlushRecorded = 0;
    int  g_cacheHitSkipFlushRecorded  = 0;
    int  g_unionExceedsCapRecorded    = 0;
    int  g_fullFlushForRedrawRecorded = 0;
    uint32_t g_unionExceedsAreaPx = 0;
    uint8_t  g_unionExceedsRectCount = 0;
    uint8_t  g_unionExceedsSourceMask = 0;
    int  g_restingFlushDecisionCounts[4] = {};
    int  g_persistedFlushDecisionCounts[4] = {};
    // Capture for ESP-Spec 3.015 §9 priority-arrow-flash synthesis test.
    Direction g_lastArrowDir   = DIR_NONE;
    uint8_t   g_lastArrowFlash = 0;
    bool      g_lastArrowMuted = false;
    uint8_t   g_lastBandsMask = 0;
    bool      g_lastBandsMuted = false;
    uint8_t   g_lastBandsFlash = 0;
    uint32_t  g_lastFrequency = 0;
    Band      g_lastFrequencyBand = BAND_NONE;
    bool      g_lastFrequencyMuted = false;
    bool      g_lastFrequencyPhoto = false;
    uint8_t   g_lastSignalBarsFront = 0;
    uint8_t   g_lastSignalBarsRear = 0;
    Band      g_lastSignalBarsBand = BAND_NONE;
    bool      g_lastSignalBarsMuted = false;
    char      g_lastTopCounterChar = '\0';
    bool      g_lastTopCounterMuted = false;
    bool      g_lastTopCounterDot = false;

    int flushDecisionIndex(PerfDisplayFlushDecisionReason reason) {
        return static_cast<int>(reason);
    }

    void resetFlushDecisionCounts() {
        for (int i = 0; i < 4; ++i) {
            g_restingFlushDecisionCounts[i] = 0;
            g_persistedFlushDecisionCounts[i] = 0;
        }
    }
}

bool V1Display::drawBandIndicators(uint8_t bands, bool muted, uint8_t flashBits) {
    g_lastBandsMask = bands;
    g_lastBandsMuted = muted;
    g_lastBandsFlash = flashBits;
    if (g_stubBandsAnnotates) {
        drawnRegion_.add(DisplayLayout::kBandsColumnRect.x,
                         DisplayLayout::kBandsColumnRect.y,
                         DisplayLayout::kBandsColumnRect.w,
                         DisplayLayout::kBandsColumnRect.h,
                         DisplayDirtyRegionSource::Bands);
        return true;
    }
    return false;
}
void V1Display::drawFrequency(uint32_t frequency, Band band, bool muted, bool photoRadar) {
    g_lastFrequency = frequency;
    g_lastFrequencyBand = band;
    g_lastFrequencyMuted = muted;
    g_lastFrequencyPhoto = photoRadar;
    if (g_stubFreqAnnotates) {
        drawnRegion_.add(DisplayLayout::kFrequencyZoneRect.x,
                         DisplayLayout::kFrequencyZoneRect.y,
                         DisplayLayout::kFrequencyZoneRect.w,
                         DisplayLayout::kFrequencyZoneRect.h,
                         DisplayDirtyRegionSource::Frequency);
    }
    if (g_stubBigRectAnnotate) {
        // Annotate a rect that spans the full canvas → guaranteed to exceed the
        // partial-flush area cap. Used to exercise the UnionExceedsCap path.
        drawnRegion_.add(0, 0,
                         static_cast<uint16_t>(SCREEN_WIDTH),
                         static_cast<uint16_t>(SCREEN_HEIGHT),
                         DisplayDirtyRegionSource::Frequency);
    }
}
void V1Display::drawVolumeZeroWarning() {}
void V1Display::drawStatusText(const char*, uint16_t) {}
void V1Display::drawBLEProxyIndicator() {}
void V1Display::drawDirectionArrow(Direction dir, bool muted, uint8_t flashBits, uint16_t) {
    g_lastArrowDir   = dir;
    g_lastArrowFlash = flashBits;
    g_lastArrowMuted = muted;
    if (g_stubArrowAnnotates) {
        const DisplayLayout::DisplayRect r =
            V1Display::arrowBoundingRect(/*raisedLayout=*/true);
        drawnRegion_.add(r.x, r.y, r.w, r.h, DisplayDirtyRegionSource::Arrows);
        arrowPaintedThisFrame_ = true;
    }
    if (g_stubArrowVisibilityChanged) {
        arrowVisibilityForceFullFlush_ = true;
    }
}
void V1Display::drawVerticalSignalBars(uint8_t front, uint8_t rear, Band band, bool muted) {
    g_lastSignalBarsFront = front;
    g_lastSignalBarsRear = rear;
    g_lastSignalBarsBand = band;
    g_lastSignalBarsMuted = muted;
}

// Partial-flush dispatch test seams: count flushRegion calls (the partial path)
// and provide a stub arrowBoundingRect (the real one lives in display_arrow.cpp
// which we do not include here).
void V1Display::flushRegion(int16_t x, int16_t y, int16_t w, int16_t h) {
    ++g_flushRegionCalls;
    g_lastFlushX = x;
    g_lastFlushY = y;
    g_lastFlushW = w;
    g_lastFlushH = h;
}
DisplayLayout::DisplayRect V1Display::arrowBoundingRect(bool raisedLayout) {
    DisplayLayout::DisplayRect r;
    r.x = 395;
    r.y = raisedLayout ? 36 : 46;
    r.w = 144;
    r.h = 130;
    return r;
}
void V1Display::drawBaseFrame() {
    if (tft_) {
        tft_->fillScreen(currentPalette_.bg);
    }
}
void V1Display::prepareFullRedrawNoClear() {}
void V1Display::drawTopCounter(char topChar, bool muted, bool dot) {
    g_lastTopCounterChar = topChar;
    g_lastTopCounterMuted = muted;
    g_lastTopCounterDot = dot;
}
void V1Display::drawTopCounterSegment7(char, bool, bool) {}
void V1Display::drawTopCounterPair(char primary, bool muted, bool primaryDot, char, bool) {
    g_lastTopCounterChar = primary;
    g_lastTopCounterMuted = muted;
    g_lastTopCounterDot = primaryDot;
}
void V1Display::drawSecondaryAlertCards(const AlertData*, int, const AlertData&, bool) {}
void V1Display::drawVolumeIndicator(uint8_t, uint8_t) {}
void V1Display::drawRssiIndicator(int) {}
void V1Display::drawMuteIcon(bool) {}
void V1Display::drawObdIndicator() {}
void V1Display::drawGpsIndicator() {}
void V1Display::drawAlpIndicator() {}
void V1Display::syncTopIndicators(uint32_t) {}
void V1Display::setObdStatus(bool, bool, bool) {}
bool V1Display::hasFreshBleContext(uint32_t) const { return true; }
void V1Display::drawProfileIndicator(int) {}
void V1Display::drawBatteryIndicator() {}
void V1Display::drawWiFiIndicator() {
    if (g_stubWifiAnnotates) {
        drawnRegion_.add(8, 145, 24, 24, DisplayDirtyRegionSource::Status);
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

void perfRecordFlushUs(uint32_t, uint32_t, bool) {}
void perfRecordDisplayUnionExceedsCap(uint32_t areaPx, uint8_t rectCount, uint8_t sourceMask) {
    g_unionExceedsAreaPx = areaPx;
    g_unionExceedsRectCount = rectCount;
    g_unionExceedsSourceMask = sourceMask;
}
void perfRecordDisplayScenarioRenderUs(uint32_t) {}
void perfRecordDisplayRenderPath(PerfDisplayRenderPath) {}
void perfRecordDisplayRedrawReason(PerfDisplayRedrawReason reason) {
    switch (reason) {
        case PerfDisplayRedrawReason::PartialRegionFlush:
            ++g_partialRegionFlushRecorded;
            break;
        case PerfDisplayRedrawReason::CacheHitSkipFlush:
            ++g_cacheHitSkipFlushRecorded;
            break;
        case PerfDisplayRedrawReason::UnionExceedsCap:
            ++g_unionExceedsCapRecorded;
            break;
        case PerfDisplayRedrawReason::FullFlushForRedraw:
            ++g_fullFlushForRedrawRecorded;
            break;
        default:
            break;
    }
}
void perfRecordDisplayRenderSubphaseUs(PerfDisplayRenderSubphase, uint32_t) {}
void perfRecordDisplayFlushDecision(PerfDisplayFlushDecisionPath path,
                                    PerfDisplayFlushDecisionReason reason) {
    const int idx = flushDecisionIndex(reason);
    if (idx < 0 || idx >= 4) {
        return;
    }
    switch (path) {
        case PerfDisplayFlushDecisionPath::Resting:
            ++g_restingFlushDecisionCounts[idx];
            break;
        case PerfDisplayFlushDecisionPath::Persisted:
            ++g_persistedFlushDecisionCounts[idx];
            break;
    }
}
void perfRecordDisplayScreenTransition(PerfDisplayScreen, PerfDisplayScreen, uint32_t) {}
PerfDisplayRenderScenario perfGetDisplayRenderScenario() {
    return PerfDisplayRenderScenario::None;
}

#include "../../src/display_update.cpp"

DisplayCorrectnessTraceEvent buildDisplayCorrectnessTraceEvent(const RenderFrame&, uint32_t) {
    return {};
}
bool displayCorrectnessTracePublish(const DisplayCorrectnessTraceEvent&) {
    return true;
}

#include "../../src/display_screens.cpp"

V1Display display;

namespace {

Arduino_Canvas* canvas() {
    return display.testCanvas();
}

void resetDisplayForTest() {
    display.~V1Display();
    new (&display) V1Display();
    display.setTestCanvas(new Arduino_Canvas(SCREEN_WIDTH, SCREEN_HEIGHT, nullptr));
    canvas()->resetCounters();
    mockMillis = 1000;
    mockMicros = 1000;
    resetFlushDecisionCounts();
}

DisplayState baselineState() {
    DisplayState state;
    state.bogeyCounterChar = '0';
    state.bogeyCounterDot = false;
    state.activeBands = BAND_NONE;
    state.signalBars = 0;
    state.muted = false;
    state.hasVolumeData = false;
    return state;
}

AlertData liveAlert() {
    return AlertData::create(BAND_KA, DIR_FRONT, 4, 0, 34700, true, true);
}

}  // namespace

void setUp() {
    resetDisplayForTest();
    g_stubArrowAnnotates  = false;
    g_stubFreqAnnotates   = false;
    g_stubBandsAnnotates  = false;
    g_stubWifiAnnotates   = false;
    g_stubBigRectAnnotate = false;
    g_stubArrowVisibilityChanged = false;
    g_flushRegionCalls    = 0;
    g_lastFlushX = -1;
    g_lastFlushY = -1;
    g_lastFlushW = -1;
    g_lastFlushH = -1;
    g_partialRegionFlushRecorded = 0;
    g_cacheHitSkipFlushRecorded  = 0;
    g_unionExceedsCapRecorded    = 0;
    g_fullFlushForRedrawRecorded = 0;
    g_unionExceedsAreaPx = 0;
    g_unionExceedsRectCount = 0;
    g_unionExceedsSourceMask = 0;
    g_lastArrowDir = DIR_NONE;
    g_lastArrowFlash = 0;
    g_lastArrowMuted = false;
    g_lastBandsMask = 0;
    g_lastBandsMuted = false;
    g_lastBandsFlash = 0;
    g_lastFrequency = 0;
    g_lastFrequencyBand = BAND_NONE;
    g_lastFrequencyMuted = false;
    g_lastFrequencyPhoto = false;
    g_lastSignalBarsFront = 0;
    g_lastSignalBarsRear = 0;
    g_lastSignalBarsBand = BAND_NONE;
    g_lastSignalBarsMuted = false;
    g_lastTopCounterChar = '\0';
    g_lastTopCounterMuted = false;
    g_lastTopCounterDot = false;
}

void tearDown() {}

void test_resting_update_same_screen_skips_full_screen_clear() {
    const DisplayState state = baselineState();

    display.update(state);
    TEST_ASSERT_EQUAL_INT(1, canvas()->getFillScreenCount());
    TEST_ASSERT_EQUAL_INT(1, canvas()->getFlushCount());
    TEST_ASSERT_EQUAL_INT(1, g_restingFlushDecisionCounts[flushDecisionIndex(PerfDisplayFlushDecisionReason::FullRedraw)]);

    canvas()->resetCounters();
    g_cacheHitSkipFlushRecorded = 0;
    resetFlushDecisionCounts();
    mockMillis += 50;
    mockMicros += 50;

    display.update(state);

    TEST_ASSERT_EQUAL_INT(0, canvas()->getFillScreenCount());
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, canvas()->getFlushCount(),
        "unchanged resting frame must not DISPLAY_FLUSH()");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_flushRegionCalls,
        "resting cache-hit frame must not call flushRegion");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_cacheHitSkipFlushRecorded,
        "unchanged resting frame must record CacheHitSkipFlush");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1,
        g_restingFlushDecisionCounts[flushDecisionIndex(PerfDisplayFlushDecisionReason::CacheHit)],
        "unchanged resting frame must attribute the no-flush decision as CacheHit");
}

void test_persisted_update_same_screen_skips_full_screen_clear() {
    const DisplayState state = baselineState();
    const AlertData alert = liveAlert();

    display.updatePersisted(alert, state);
    TEST_ASSERT_EQUAL_INT(1, canvas()->getFillScreenCount());
    TEST_ASSERT_EQUAL_INT(1, canvas()->getFlushCount());
    TEST_ASSERT_EQUAL_INT(1, g_persistedFlushDecisionCounts[flushDecisionIndex(PerfDisplayFlushDecisionReason::FullRedraw)]);

    canvas()->resetCounters();
    g_cacheHitSkipFlushRecorded = 0;
    resetFlushDecisionCounts();
    mockMillis += 50;
    mockMicros += 50;

    display.updatePersisted(alert, state);

    TEST_ASSERT_EQUAL_INT(0, canvas()->getFillScreenCount());
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, canvas()->getFlushCount(),
        "unchanged persisted frame must not DISPLAY_FLUSH()");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_flushRegionCalls,
        "persisted cache-hit frame must not call flushRegion");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_cacheHitSkipFlushRecorded,
        "unchanged persisted frame must record CacheHitSkipFlush");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1,
        g_persistedFlushDecisionCounts[flushDecisionIndex(PerfDisplayFlushDecisionReason::CacheHit)],
        "unchanged persisted frame must attribute the no-flush decision as CacheHit");
}

void test_resting_update_same_screen_flushes_when_leaf_repaints() {
    const DisplayState state = baselineState();

    display.update(state);
    canvas()->resetCounters();
    g_cacheHitSkipFlushRecorded = 0;
    g_flushRegionCalls = 0;
    resetFlushDecisionCounts();
    mockMillis += 50;
    mockMicros += 50;

    g_stubBandsAnnotates = true;
    display.update(state);

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, canvas()->getFlushCount(),
        "resting frame with any painted leaf must use safe full flush");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_flushRegionCalls,
        "resting update must not introduce partial flushes");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_cacheHitSkipFlushRecorded,
        "painted resting frame must not record CacheHitSkipFlush");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1,
        g_restingFlushDecisionCounts[flushDecisionIndex(PerfDisplayFlushDecisionReason::Painted)],
        "painted resting frame must attribute the full flush to a leaf repaint");
}

void test_persisted_update_same_screen_flushes_when_leaf_repaints() {
    const DisplayState state = baselineState();
    const AlertData alert = liveAlert();

    display.updatePersisted(alert, state);
    canvas()->resetCounters();
    g_cacheHitSkipFlushRecorded = 0;
    g_flushRegionCalls = 0;
    resetFlushDecisionCounts();
    mockMillis += 50;
    mockMicros += 50;

    g_stubFreqAnnotates = true;
    display.updatePersisted(alert, state);

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, canvas()->getFlushCount(),
        "persisted frame with any painted leaf must use safe full flush");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_flushRegionCalls,
        "persisted update must not introduce partial flushes");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_cacheHitSkipFlushRecorded,
        "painted persisted frame must not record CacheHitSkipFlush");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1,
        g_persistedFlushDecisionCounts[flushDecisionIndex(PerfDisplayFlushDecisionReason::Painted)],
        "painted persisted frame must attribute the full flush to a leaf repaint");
}

void test_resting_pending_external_draw_records_pending_external_reason() {
    const DisplayState state = baselineState();

    display.update(state);
    canvas()->resetCounters();
    g_cacheHitSkipFlushRecorded = 0;
    g_flushRegionCalls = 0;
    resetFlushDecisionCounts();
    mockMillis += 50;
    mockMicros += 50;

    g_stubBandsAnnotates = true;
    display.ut_drawBandIndicators(BAND_KA, false);
    g_stubBandsAnnotates = false;

    display.update(state);

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, canvas()->getFlushCount(),
        "resting pending external draw must force the existing safe full flush");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_flushRegionCalls,
        "resting pending external draw must not introduce partial flushes");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1,
        g_restingFlushDecisionCounts[flushDecisionIndex(PerfDisplayFlushDecisionReason::PendingExternal)],
        "resting pending external draw must be attributed separately from leaf repaint");
}

void test_stealth_same_speed_skips_unchanged_full_flush() {
    display.showStealth(42.2f, true);
    TEST_ASSERT_EQUAL_INT(1, canvas()->getFillScreenCount());
    TEST_ASSERT_EQUAL_INT(1, canvas()->getFlushCount());

    canvas()->resetCounters();
    g_cacheHitSkipFlushRecorded = 0;
    mockMillis += 50;
    mockMicros += 50;

    display.showStealth(42.4f, true);  // same displayed text: "42 MPH"

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, canvas()->getFillScreenCount(),
        "unchanged stealth speed text must not repaint the full canvas");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, canvas()->getFlushCount(),
        "unchanged stealth speed text must not DISPLAY_FLUSH()");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_cacheHitSkipFlushRecorded,
        "unchanged stealth frame must record CacheHitSkipFlush");
}

void test_stealth_speed_text_change_full_flushes() {
    display.showStealth(42.2f, true);
    canvas()->resetCounters();
    g_cacheHitSkipFlushRecorded = 0;
    mockMillis += 50;
    mockMicros += 50;

    display.showStealth(43.0f, true);

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, canvas()->getFillScreenCount(),
        "changed stealth speed text must repaint the blank speed screen");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, canvas()->getFlushCount(),
        "changed stealth speed text must full-flush the repaint");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_cacheHitSkipFlushRecorded,
        "changed stealth frame must not use the cache-hit path");
}

void test_stealth_pending_external_draw_forces_clear_flush() {
    display.showStealth(42.2f, true);
    canvas()->resetCounters();
    g_cacheHitSkipFlushRecorded = 0;
    mockMillis += 50;
    mockMicros += 50;

    g_stubBandsAnnotates = true;
    display.ut_drawBandIndicators(BAND_KA, false);
    g_stubBandsAnnotates = false;

    display.showStealth(42.2f, true);

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, canvas()->getFillScreenCount(),
        "pending external indicator draws must be erased even if stealth text is unchanged");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, canvas()->getFlushCount(),
        "pending external indicator draws must force a safe full stealth flush");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_cacheHitSkipFlushRecorded,
        "pending external draws must not be hidden by cache-hit skip");
}

void test_resting_after_stealth_forces_full_redraw() {
    const DisplayState state = baselineState();

    display.showStealth(42.2f, true);
    canvas()->resetCounters();
    g_cacheHitSkipFlushRecorded = 0;
    mockMillis += 50;
    mockMicros += 50;

    display.update(state);

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, canvas()->getFillScreenCount(),
        "leaving stealth for normal resting must fully restore the resting frame");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, canvas()->getFlushCount(),
        "leaving stealth for normal resting must full-flush the restored frame");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_cacheHitSkipFlushRecorded,
        "resting restore after stealth must not be treated as a cache hit");
}

void test_live_update_same_screen_skips_full_screen_clear() {
    DisplayState state = baselineState();
    state.activeBands = BAND_KA;
    state.arrows = DIR_FRONT;
    state.priorityArrow = DIR_FRONT;
    state.signalBars = 4;
    const AlertData alert = liveAlert();

    display.update(alert, &alert, 1, state);
    TEST_ASSERT_EQUAL_INT(1, canvas()->getFillScreenCount());

    canvas()->resetCounters();
    mockMillis += 50;
    mockMicros += 50;

    display.update(alert, &alert, 1, state);

    TEST_ASSERT_EQUAL_INT(0, canvas()->getFillScreenCount());
}

// ============================================================================
// Region-union partial-flush dispatch
// (docs/plans/PARTIAL_FLUSH_REGION_UNION_20260422.md)
//
// Dispatch order in V1Display::update(priority, ...):
//   1. needsFullRedraw             → DISPLAY_FLUSH + FullFlushForRedraw
//   2. drawnRegion_.empty()        → no flush + CacheHitSkipFlush
//   3. blink bits active           → DISPLAY_FLUSH + FullFlushForRedraw
//   4. union area ≥ cap            → DISPLAY_FLUSH + UnionExceedsCap
//   5. arrow visibility changed    → DISPLAY_FLUSH + FullFlushForRedraw
//   6. otherwise                   → flushRegion + PartialRegionFlush
// ============================================================================

// Helper: run a live "enter Live" frame then reset counters so subsequent
// assertions target only the frame under test.
namespace {
void enterLiveAndResetCounters(const AlertData& alert, const DisplayState& state) {
    display.update(alert, &alert, 1, state);
    canvas()->resetCounters();
    g_flushRegionCalls = 0;
    g_lastFlushX = -1;
    g_lastFlushY = -1;
    g_lastFlushW = -1;
    g_lastFlushH = -1;
    g_partialRegionFlushRecorded = 0;
    g_cacheHitSkipFlushRecorded  = 0;
    g_unionExceedsCapRecorded    = 0;
    g_fullFlushForRedrawRecorded = 0;
    g_unionExceedsAreaPx = 0;
    g_unionExceedsRectCount = 0;
    g_unionExceedsSourceMask = 0;
    mockMillis += 50;
    mockMicros += 50;
}
}  // namespace

void test_partial_region_flush_when_only_arrow_annotates() {
    DisplayState state = baselineState();
    state.activeBands = BAND_KA;
    state.arrows = DIR_FRONT;
    state.priorityArrow = DIR_FRONT;
    state.signalBars = 4;
    const AlertData alert = liveAlert();

    // Frame 1: entering Live → needsFullRedraw=true → full flush
    display.update(alert, &alert, 1, state);
    TEST_ASSERT_EQUAL_INT(1, canvas()->getFlushCount());
    TEST_ASSERT_EQUAL_INT(0, g_flushRegionCalls);
    TEST_ASSERT_EQUAL_INT(1, g_fullFlushForRedrawRecorded);

    canvas()->resetCounters();
    g_flushRegionCalls = 0;
    g_partialRegionFlushRecorded = 0;
    g_cacheHitSkipFlushRecorded  = 0;
    g_unionExceedsCapRecorded    = 0;
    g_fullFlushForRedrawRecorded = 0;
    mockMillis += 50;
    mockMicros += 50;

    // Frame 2: already in Live, only the arrow leaf annotates → partial path.
    g_stubArrowAnnotates = true;
    g_stubFreqAnnotates  = false;
    display.update(alert, &alert, 1, state);

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, canvas()->getFlushCount(),
        "arrow-only change must not full-flush");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_flushRegionCalls,
        "arrow-only change must call flushRegion exactly once");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_partialRegionFlushRecorded,
        "arrow-only change must record PartialRegionFlush");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_fullFlushForRedrawRecorded,
        "steady-state frame must not record FullFlushForRedraw");
}

void test_empty_region_skips_flush_entirely() {
    DisplayState state = baselineState();
    state.activeBands = BAND_KA;
    state.arrows = DIR_FRONT;
    state.priorityArrow = DIR_FRONT;
    state.signalBars = 4;
    const AlertData alert = liveAlert();

    enterLiveAndResetCounters(alert, state);

    // Frame 2: no leaf annotates — full cache hit everywhere.
    g_stubArrowAnnotates  = false;
    g_stubFreqAnnotates   = false;
    g_stubBigRectAnnotate = false;
    display.update(alert, &alert, 1, state);

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, canvas()->getFlushCount(),
        "empty drawn region must not DISPLAY_FLUSH()");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_flushRegionCalls,
        "empty drawn region must not call flushRegion");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_cacheHitSkipFlushRecorded,
        "empty drawn region must record CacheHitSkipFlush");
}

void test_blink_cache_hit_still_skips_flush() {
    DisplayState state = baselineState();
    state.activeBands = BAND_KA;
    state.arrows = DIR_FRONT;
    state.priorityArrow = DIR_FRONT;
    state.signalBars = 4;
    state.flashBits = 0x20;
    const AlertData alert = liveAlert();

    enterLiveAndResetCounters(alert, state);

    // Flash bits alone are not a reason to push the framebuffer. V1 packets can
    // arrive faster than the 96 ms blink phase; if no leaf painted pixels, skip.
    g_stubArrowAnnotates  = false;
    g_stubFreqAnnotates   = false;
    g_stubBigRectAnnotate = false;
    display.update(alert, &alert, 1, state);

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, canvas()->getFlushCount(),
        "blink cache-hit frame must not DISPLAY_FLUSH()");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_flushRegionCalls,
        "blink cache-hit frame must not call flushRegion");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_cacheHitSkipFlushRecorded,
        "blink cache-hit frame must record CacheHitSkipFlush");
}

void test_blink_arrow_change_full_flushes_instead_of_partial_region() {
    DisplayState state = baselineState();
    state.activeBands = BAND_KA;
    state.arrows = DIR_FRONT;
    state.priorityArrow = DIR_FRONT;
    state.signalBars = 4;
    state.flashBits = 0x20;
    const AlertData alert = liveAlert();

    enterLiveAndResetCounters(alert, state);

    // Blink-bearing frames that do paint pixels must avoid the AXS15231B small
    // partial-window path; diag14 showed the framebuffer toggled correctly but
    // repeated path=4 updates did not visibly blink on hardware.
    g_stubArrowAnnotates = true;
    g_stubFreqAnnotates  = false;
    display.update(alert, &alert, 1, state);

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, canvas()->getFlushCount(),
        "dirty blink frame must full-flush for reliable panel latch");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_flushRegionCalls,
        "dirty blink frame must not use partial flushRegion");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_fullFlushForRedrawRecorded,
        "dirty blink frame must record the full-flush path");
}

void test_arrow_visibility_change_full_flushes_instead_of_partial_region() {
    DisplayState state = baselineState();
    state.activeBands = BAND_KA;
    state.arrows = DIR_SIDE;
    state.priorityArrow = DIR_SIDE;
    state.signalBars = 4;
    state.flashBits = 0;
    const AlertData alert = liveAlert();

    enterLiveAndResetCounters(alert, state);

    // arrow_diag16 showed the framebuffer repainting active->resting arrows
    // correctly (fill=0x1082) after a V1 blink-off black phase, but two such
    // non-blink direction changes went through path=4 small-window partial
    // flushes. Those are infrequent and need the reliable canvas full-push.
    g_stubArrowAnnotates = true;
    g_stubArrowVisibilityChanged = true;
    display.update(alert, &alert, 1, state);

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, canvas()->getFlushCount(),
        "arrow visibility change must full-flush for reliable resting-glyph restore");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_flushRegionCalls,
        "arrow visibility change must not use partial flushRegion");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_fullFlushForRedrawRecorded,
        "arrow visibility change must record the full-flush path");
}

void test_union_exceeds_cap_takes_full_flush_path() {
    DisplayState state = baselineState();
    state.activeBands = BAND_KA;
    state.arrows = DIR_FRONT;
    state.priorityArrow = DIR_FRONT;
    state.signalBars = 4;
    const AlertData alert = liveAlert();

    enterLiveAndResetCounters(alert, state);

    // Frame 2: one leaf annotates the entire canvas → union area ≥ cap.
    g_stubBigRectAnnotate = true;
    display.update(alert, &alert, 1, state);

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, canvas()->getFlushCount(),
        "union exceeding cap must fall back to full DISPLAY_FLUSH");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_flushRegionCalls,
        "union exceeding cap must not call flushRegion");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_unionExceedsCapRecorded,
        "union exceeding cap must record UnionExceedsCap");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_fullFlushForRedrawRecorded,
        "steady-state over-cap must not record FullFlushForRedraw");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        static_cast<uint32_t>(SCREEN_WIDTH) * static_cast<uint32_t>(SCREEN_HEIGHT),
        g_unionExceedsAreaPx,
        "UnionExceedsCap instrumentation must record the dirty union area");
    TEST_ASSERT_TRUE_MESSAGE(
        (g_unionExceedsSourceMask & DisplayDirtyRegionSource::Frequency) != 0,
        "UnionExceedsCap instrumentation must include the contributing source mask");
}

void test_union_exceeds_cap_source_mask_combines_dirty_leaves() {
    DisplayState state = baselineState();
    state.activeBands = BAND_KA;
    state.arrows = DIR_FRONT;
    state.priorityArrow = DIR_FRONT;
    state.signalBars = 4;
    const AlertData alert = liveAlert();

    enterLiveAndResetCounters(alert, state);

    g_stubBigRectAnnotate = true;
    g_stubArrowAnnotates = true;
    display.update(alert, &alert, 1, state);

    TEST_ASSERT_EQUAL_INT(1, g_unionExceedsCapRecorded);
    TEST_ASSERT_EQUAL_UINT8(2, g_unionExceedsRectCount);
    TEST_ASSERT_TRUE((g_unionExceedsSourceMask & DisplayDirtyRegionSource::Frequency) != 0);
    TEST_ASSERT_TRUE((g_unionExceedsSourceMask & DisplayDirtyRegionSource::Arrows) != 0);
}

void test_over_cap_disjoint_non_arrow_rects_use_multi_rect_partial_flush() {
    DisplayState state = baselineState();
    state.activeBands = BAND_KA;
    state.arrows = DIR_FRONT;
    state.priorityArrow = DIR_FRONT;
    state.signalBars = 4;
    const AlertData alert = liveAlert();

    enterLiveAndResetCounters(alert, state);

    // Frequency plus a bottom-left status icon creates a >50%-canvas union
    // bbox because of the vertical gap between them.  The item rects
    // themselves are small and non-arrow, so the multi-rect dispatcher should
    // flush each owned space instead of falling back to a full canvas push.
    g_stubFreqAnnotates = true;
    g_stubWifiAnnotates = true;
    display.update(alert, &alert, 1, state);

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, canvas()->getFlushCount(),
        "safe disjoint item rects over the union cap must not full-flush");
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, g_flushRegionCalls,
        "safe disjoint item rects should flush as two owned windows");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_partialRegionFlushRecorded,
        "multi-rect dispatch still records the partial-flush decision once");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_unionExceedsCapRecorded,
        "multi-rect dispatch should avoid the UnionExceedsCap fallback");
}

void test_needs_full_redraw_always_records_full_flush_for_redraw() {
    DisplayState state = baselineState();
    state.activeBands = BAND_KA;
    state.arrows = DIR_FRONT;
    state.priorityArrow = DIR_FRONT;
    state.signalBars = 4;
    const AlertData alert = liveAlert();

    // First frame into Live: needsFullRedraw=true regardless of leaf annotations.
    g_stubArrowAnnotates = true;
    g_stubFreqAnnotates  = false;
    display.update(alert, &alert, 1, state);

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, canvas()->getFlushCount(),
        "entering Live must full-flush even when only arrow annotates");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_flushRegionCalls,
        "entering Live must not take the partial-flush path");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_fullFlushForRedrawRecorded,
        "entering Live must record FullFlushForRedraw");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_partialRegionFlushRecorded,
        "entering Live must not record PartialRegionFlush");
}

void test_bands_plus_frequency_still_under_cap_takes_partial_flush() {
    DisplayState state = baselineState();
    state.activeBands = BAND_KA;
    state.arrows = DIR_FRONT;
    state.priorityArrow = DIR_FRONT;
    state.signalBars = 4;
    const AlertData alert = liveAlert();

    enterLiveAndResetCounters(alert, state);

    // Frame 2: bands column + frequency zone. Both sit in the primary-zone Y
    // range (y=20..115). Union bbox is x=[0..440] × y=[20..115] = 440×95 =
    // 41800 px, well under the 55040-px cap (SCREEN_WIDTH*SCREEN_HEIGHT/2).
    // This exercises the common multi-zone steady-state where band + freq both
    // animated but nothing else did — must take the partial path.
    //
    // NOTE: arrow + frequency *does* exceed the cap in production (raised
    // arrow rect spans y=0..148 while the freq zone ends at y=115; the union
    // is nearly the full panel). That case correctly falls into the UnionExceeds
    // Cap branch exercised by test_union_exceeds_cap_takes_full_flush_path.
    g_stubBandsAnnotates = true;
    g_stubFreqAnnotates  = true;
    display.update(alert, &alert, 1, state);

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, canvas()->getFlushCount(),
        "multi-zone under-cap must not full-flush");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_flushRegionCalls,
        "multi-zone under-cap must call flushRegion once");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_partialRegionFlushRecorded,
        "multi-zone under-cap must record PartialRegionFlush");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_unionExceedsCapRecorded,
        "multi-zone under-cap must not record UnionExceedsCap");
}

void test_drawn_region_resets_between_frames() {
    DisplayState state = baselineState();
    state.activeBands = BAND_KA;
    state.arrows = DIR_FRONT;
    state.priorityArrow = DIR_FRONT;
    state.signalBars = 4;
    const AlertData alert = liveAlert();

    enterLiveAndResetCounters(alert, state);

    // Frame 2: arrow annotates → partial flush
    g_stubArrowAnnotates = true;
    display.update(alert, &alert, 1, state);
    TEST_ASSERT_EQUAL_INT(1, g_partialRegionFlushRecorded);

    canvas()->resetCounters();
    g_flushRegionCalls = 0;
    g_partialRegionFlushRecorded = 0;
    g_cacheHitSkipFlushRecorded  = 0;
    mockMillis += 50;
    mockMicros += 50;

    // Frame 3: no annotations at all → region should reset and cache-hit path
    // fires (NOT another partial flush covering last frame's stale arrow rect).
    g_stubArrowAnnotates = false;
    g_stubFreqAnnotates  = false;
    display.update(alert, &alert, 1, state);

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_flushRegionCalls,
        "drawnRegion_ must reset between frames — no stale arrow flush");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_partialRegionFlushRecorded,
        "cache-hit frame must not record PartialRegionFlush");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_cacheHitSkipFlushRecorded,
        "cache-hit frame must record CacheHitSkipFlush");
}

void test_live_pending_external_region_flushes_on_cache_hit_frame() {
    DisplayState state = baselineState();
    state.activeBands = BAND_KA;
    state.arrows = DIR_FRONT;
    state.priorityArrow = DIR_FRONT;
    state.signalBars = 4;
    const AlertData alert = liveAlert();

    enterLiveAndResetCounters(alert, state);

    // Simulate an external setter drawing into the shared framebuffer between
    // live frames. The next live frame's leaves all cache-hit, so preserving
    // this pending region is the only reason a flush should happen.
    g_stubBandsAnnotates = true;
    display.ut_drawBandIndicators(BAND_KA, false);
    g_stubBandsAnnotates = false;

    display.update(alert, &alert, 1, state);

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, canvas()->getFlushCount(),
        "pending external indicator region under the cap must not full-flush");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_flushRegionCalls,
        "pending external indicator region must be flushed exactly once");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_partialRegionFlushRecorded,
        "pending external indicator region should use the normal partial dispatch");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_cacheHitSkipFlushRecorded,
        "pending external indicator region must not be hidden by cache-hit skip");
    TEST_ASSERT_EQUAL_INT(DisplayLayout::kBandsColumnRect.x, g_lastFlushX);
    TEST_ASSERT_EQUAL_INT(DisplayLayout::kBandsColumnRect.y, g_lastFlushY);
    TEST_ASSERT_EQUAL_INT(DisplayLayout::kBandsColumnRect.w, g_lastFlushW);
    TEST_ASSERT_EQUAL_INT(DisplayLayout::kBandsColumnRect.h, g_lastFlushH);

    canvas()->resetCounters();
    g_flushRegionCalls = 0;
    g_partialRegionFlushRecorded = 0;
    g_cacheHitSkipFlushRecorded = 0;
    mockMillis += 50;
    mockMicros += 50;

    display.update(alert, &alert, 1, state);

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_flushRegionCalls,
        "external pending region must be consumed after one flush, not re-flushed");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_cacheHitSkipFlushRecorded,
        "following unchanged live frame should return to cache-hit skip");
}

void test_live_pending_external_region_unions_with_frame_paint() {
    DisplayState state = baselineState();
    state.activeBands = BAND_KA;
    state.arrows = DIR_FRONT;
    state.priorityArrow = DIR_FRONT;
    state.signalBars = 4;
    const AlertData alert = liveAlert();

    enterLiveAndResetCounters(alert, state);

    g_stubBandsAnnotates = true;
    display.ut_drawBandIndicators(BAND_KA, false);
    g_stubBandsAnnotates = false;

    // The live frame itself repaints the frequency zone. The dispatch region
    // must include both the externally painted band column and this frame's
    // frequency paint, otherwise one of them could remain stale on the panel.
    g_stubFreqAnnotates = true;
    display.update(alert, &alert, 1, state);

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, canvas()->getFlushCount(),
        "bands+frequency union stays under the full-flush area cap");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_flushRegionCalls,
        "external+live paint union should dispatch as one partial flush");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_partialRegionFlushRecorded,
        "external+live paint union must record PartialRegionFlush");
    TEST_ASSERT_EQUAL_INT(0, g_lastFlushX);
    TEST_ASSERT_EQUAL_INT(DisplayLayout::PRIMARY_ZONE_Y, g_lastFlushY);
    TEST_ASSERT_EQUAL_INT(DisplayLayout::BAND_COLUMN_WIDTH + DisplayLayout::CONTENT_AVAILABLE_WIDTH,
                          g_lastFlushW);
    TEST_ASSERT_EQUAL_INT(DisplayLayout::PRIMARY_ZONE_HEIGHT, g_lastFlushH);
}

void test_live_invalid_priority_preserves_pending_external_region() {
    DisplayState state = baselineState();
    state.activeBands = BAND_KA;
    state.arrows = DIR_FRONT;
    state.priorityArrow = DIR_FRONT;
    state.signalBars = 4;
    const AlertData alert = liveAlert();

    enterLiveAndResetCounters(alert, state);

    g_stubBandsAnnotates = true;
    display.ut_drawBandIndicators(BAND_KA, false);
    g_stubBandsAnnotates = false;

    AlertData invalidAlert;
    display.update(invalidAlert, nullptr, 0, state);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, canvas()->getFlushCount(),
        "invalid priority path must not flush by itself");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_flushRegionCalls,
        "invalid priority path must not consume the pending external region");

    display.update(alert, &alert, 1, state);

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_flushRegionCalls,
        "next valid live frame must still flush the preserved external region");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_partialRegionFlushRecorded,
        "preserved external region should use partial dispatch");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_cacheHitSkipFlushRecorded,
        "preserved external region must not be lost to cache-hit skip");
}

// ============================================================================
// V1 reports the priority-arrow blink directly via image1 vs image2 in the
// InfDisplayData packet; packet_parser.cpp computes
// state.flashBits = image1 & ~image2 & 0xE0. display_update.cpp must pass
// that byte through to drawDirectionArrow() unchanged. Synthesizing a flash
// bit when alertCount > 1 would force spurious blinks during 2-alert windows
// where V1 explicitly reports "no blink", so the tests below lock in the
// pass-through behavior.
// ============================================================================

void test_live_single_alert_passes_zero_flash_when_v1_reports_steady() {
    DisplayState state = baselineState();
    state.activeBands = BAND_KA;
    state.arrows = DIR_FRONT;
    state.priorityArrow = DIR_FRONT;
    state.signalBars = 4;
    state.flashBits = 0;
    const AlertData alert = liveAlert();

    display.update(alert, &alert, 1, state);

    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, g_lastArrowFlash,
        "V1 reported steady (flashBits=0) → drawDirectionArrow must see 0");
}

void test_live_multi_alert_does_not_synthesize_priority_arrow_flash() {
    DisplayState state = baselineState();
    state.activeBands = static_cast<Band>(BAND_KA | BAND_K);
    state.arrows = static_cast<Direction>(DIR_FRONT | DIR_REAR);
    state.priorityArrow = DIR_FRONT;
    state.signalBars = 4;
    state.flashBits = 0;  // V1 says: do not blink

    const AlertData priority = AlertData::create(BAND_KA, DIR_FRONT, 4, 0, 34700, true, true);
    const AlertData secondary = AlertData::create(BAND_K, DIR_REAR, 3, 0, 24150, true, false);
    const AlertData alerts[2] = {priority, secondary};

    display.update(priority, alerts, 2, state);

    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, g_lastArrowFlash & 0xE0,
        "alertCount > 1 must NOT force a priority-arrow blink — V1 itself decides");
}

void test_live_passes_v1_reported_flash_bits_through_unchanged() {
    DisplayState state = baselineState();
    state.activeBands = static_cast<Band>(BAND_KA | BAND_K);
    state.arrows = static_cast<Direction>(DIR_FRONT | DIR_REAR);
    state.priorityArrow = DIR_FRONT;
    state.signalBars = 4;
    state.flashBits = 0x80;  // V1 explicitly asks rear arrow to blink

    const AlertData priority = AlertData::create(BAND_KA, DIR_FRONT, 4, 0, 34700, true, true);
    const AlertData secondary = AlertData::create(BAND_K, DIR_REAR, 3, 0, 24150, true, false);
    const AlertData alerts[2] = {priority, secondary};

    display.update(priority, alerts, 2, state);

    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0x80, g_lastArrowFlash & 0xE0,
        "V1's reported flash bits must reach drawDirectionArrow unchanged");
}

void test_render_frame_v1_live_drives_native_leaf_outputs() {
    DisplayState state = baselineState();
    state.activeBands = static_cast<Band>(BAND_KA | BAND_K);
    state.arrows = static_cast<Direction>(DIR_FRONT | DIR_REAR);
    state.priorityArrow = DIR_REAR;
    state.signalBars = 5;
    state.muted = true;
    state.flashBits = 0x80;
    state.bandFlashBits = 0x02;
    state.bogeyCounterChar = '3';
    state.bogeyCounterDot = true;

    const AlertData priority = AlertData::create(BAND_KA, DIR_REAR, 5, 0, 34700, true, true);
    RenderFrame frame{};
    frame.primaryKind = RenderFramePrimaryKind::V1_LIVE;
    frame.primaryState = state;
    frame.v1Priority = priority;
    frame.cardCount = 1;
    frame.cards[0].kind = RenderFrameCard::Kind::V1;
    frame.cards[0].v1Alert = priority;

    display.renderFrame(frame);

    TEST_ASSERT_EQUAL_UINT32_MESSAGE(34700u, g_lastFrequency,
        "V1 live render frame must pass parsed frequency into the native draw path");
    TEST_ASSERT_EQUAL_UINT8(BAND_KA, g_lastFrequencyBand);
    TEST_ASSERT_TRUE(g_lastFrequencyMuted);
    TEST_ASSERT_FALSE(g_lastFrequencyPhoto);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(BAND_KA | BAND_K), g_lastBandsMask);
    TEST_ASSERT_TRUE(g_lastBandsMuted);
    TEST_ASSERT_EQUAL_UINT8(0x02, g_lastBandsFlash);
    TEST_ASSERT_EQUAL_UINT8(5, g_lastSignalBarsFront);
    TEST_ASSERT_EQUAL_UINT8(5, g_lastSignalBarsRear);
    TEST_ASSERT_EQUAL_UINT8(BAND_KA, g_lastSignalBarsBand);
    TEST_ASSERT_TRUE(g_lastSignalBarsMuted);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(DIR_FRONT | DIR_REAR), g_lastArrowDir);
    TEST_ASSERT_EQUAL_UINT8(0x80, g_lastArrowFlash);
    TEST_ASSERT_TRUE(g_lastArrowMuted);
    TEST_ASSERT_EQUAL_CHAR('3', g_lastTopCounterChar);
    TEST_ASSERT_TRUE(g_lastTopCounterMuted);
    TEST_ASSERT_TRUE(g_lastTopCounterDot);
}

void test_render_frame_alp_live_drives_laser_owner_outputs() {
    DisplayState state = baselineState();
    state.activeBands = BAND_LASER;
    state.arrows = DIR_REAR;
    state.priorityArrow = DIR_REAR;
    state.signalBars = 6;
    state.muted = false;
    state.bogeyCounterChar = 'L';

    RenderFrame frame{};
    frame.primaryKind = RenderFramePrimaryKind::ALP_LIVE;
    frame.primaryState = state;
    frame.alpPrimary.active = true;
    frame.alpPrimary.direction = AlpLaserDirection::REAR;

    display.renderFrame(frame);

    TEST_ASSERT_EQUAL_UINT32(0u, g_lastFrequency);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(BAND_LASER, g_lastFrequencyBand,
        "ALP-owned frame must enter the native draw path as a laser primary");
    TEST_ASSERT_FALSE_MESSAGE(g_lastFrequencyMuted,
        "ALP-owned laser presentation must not inherit V1 muted state");
    TEST_ASSERT_EQUAL_UINT8(BAND_LASER, g_lastBandsMask);
    TEST_ASSERT_EQUAL_UINT8(6, g_lastSignalBarsFront);
    TEST_ASSERT_EQUAL_UINT8(6, g_lastSignalBarsRear);
    TEST_ASSERT_EQUAL_UINT8(BAND_LASER, g_lastSignalBarsBand);
    TEST_ASSERT_EQUAL_UINT8(DIR_REAR, g_lastArrowDir);
    TEST_ASSERT_FALSE(g_lastArrowMuted);
    TEST_ASSERT_EQUAL_CHAR('L', g_lastTopCounterChar);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_resting_update_same_screen_skips_full_screen_clear);
    RUN_TEST(test_persisted_update_same_screen_skips_full_screen_clear);
    RUN_TEST(test_resting_update_same_screen_flushes_when_leaf_repaints);
    RUN_TEST(test_persisted_update_same_screen_flushes_when_leaf_repaints);
    RUN_TEST(test_resting_pending_external_draw_records_pending_external_reason);
    RUN_TEST(test_stealth_same_speed_skips_unchanged_full_flush);
    RUN_TEST(test_stealth_speed_text_change_full_flushes);
    RUN_TEST(test_stealth_pending_external_draw_forces_clear_flush);
    RUN_TEST(test_resting_after_stealth_forces_full_redraw);
    RUN_TEST(test_live_update_same_screen_skips_full_screen_clear);
    RUN_TEST(test_partial_region_flush_when_only_arrow_annotates);
    RUN_TEST(test_empty_region_skips_flush_entirely);
    RUN_TEST(test_blink_cache_hit_still_skips_flush);
    RUN_TEST(test_blink_arrow_change_full_flushes_instead_of_partial_region);
    RUN_TEST(test_arrow_visibility_change_full_flushes_instead_of_partial_region);
    RUN_TEST(test_union_exceeds_cap_takes_full_flush_path);
    RUN_TEST(test_union_exceeds_cap_source_mask_combines_dirty_leaves);
    RUN_TEST(test_over_cap_disjoint_non_arrow_rects_use_multi_rect_partial_flush);
    RUN_TEST(test_needs_full_redraw_always_records_full_flush_for_redraw);
    RUN_TEST(test_bands_plus_frequency_still_under_cap_takes_partial_flush);
    RUN_TEST(test_drawn_region_resets_between_frames);
    RUN_TEST(test_live_pending_external_region_flushes_on_cache_hit_frame);
    RUN_TEST(test_live_pending_external_region_unions_with_frame_paint);
    RUN_TEST(test_live_invalid_priority_preserves_pending_external_region);
    RUN_TEST(test_live_single_alert_passes_zero_flash_when_v1_reports_steady);
    RUN_TEST(test_live_multi_alert_does_not_synthesize_priority_arrow_flash);
    RUN_TEST(test_live_passes_v1_reported_flash_bits_through_unchanged);
    RUN_TEST(test_render_frame_v1_live_drives_native_leaf_outputs);
    RUN_TEST(test_render_frame_alp_live_drives_laser_owner_outputs);
    return UNITY_END();
}
