#include <unity.h>

#include "../mocks/display_driver.h"
#include "../mocks/Arduino.h"
#include "../mocks/settings.h"
#include "../mocks/packet_parser.h"

#ifndef ARDUINO
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
SerialClass Serial;
#endif

#include "../../src/modules/alp/alp_runtime_module.h"
#include "../../src/display.h"
#include "../../src/modules/display/display_correctness_trace.cpp"
#define DISPLAY_RENDER_FRAME_ONLY 1
#include "../../src/display_update.cpp"
#undef DISPLAY_RENDER_FRAME_ONLY

SettingsManager settingsManager;
V1Display* g_displayInstance = nullptr;

PerfDisplayRenderScenario perfGetDisplayRenderScenario() {
    return PerfDisplayRenderScenario::None;
}

static int g_idleCalls = 0;
static int g_liveCalls = 0;
static int g_persistedCalls = 0;
static AlertData g_lastPriority{};
static int g_lastAlertCount = 0;
static bool g_lastAlertsWasNull = true;
static AlertData g_lastFirstAlert{};
static DisplayState g_lastState{};

V1Display::V1Display() { g_displayInstance = this; }
V1Display::~V1Display() = default;

void V1Display::update(const DisplayState& state) {
    ++g_idleCalls;
    g_lastState = state;
}

void V1Display::update(const AlertData& priority,
                       const AlertData* allAlerts,
                       int alertCount,
                       const DisplayState& state) {
    ++g_liveCalls;
    g_lastPriority = priority;
    g_lastAlertCount = alertCount;
    g_lastAlertsWasNull = (allAlerts == nullptr);
    g_lastFirstAlert = (allAlerts != nullptr && alertCount > 0) ? allAlerts[0] : AlertData{};
    g_lastState = state;
}

void V1Display::updatePersisted(const AlertData& alert, const DisplayState& state) {
    ++g_persistedCalls;
    g_lastPriority = alert;
    g_lastState = state;
}

void V1Display::showStealth(float /*speedMph*/, bool /*speedValid*/) {
    ++g_idleCalls;  // Stealth is an IDLE-variant path; reuse the idle counter
}

static int g_clearCalls = 0;
void V1Display::clear() {
    ++g_clearCalls;
}

static void resetCounters() {
    displayCorrectnessTraceReset();
    g_idleCalls = 0;
    g_liveCalls = 0;
    g_persistedCalls = 0;
    g_clearCalls = 0;
    g_lastPriority = AlertData{};
    g_lastAlertCount = 0;
    g_lastAlertsWasNull = true;
    g_lastFirstAlert = AlertData{};
    g_lastState = DisplayState{};
}

static AlertData makeAlert(Band band, Direction direction, uint32_t frequency) {
    return AlertData::create(band, direction, 5, 0, frequency, true, true);
}

static DisplayState makeState() {
    DisplayState state{};
    state.activeBands = BAND_KA;
    state.arrows = DIR_FRONT;
    state.priorityArrow = DIR_FRONT;
    state.signalBars = 5;
    return state;
}

void setUp() {
    resetCounters();
}

void tearDown() {}

void test_render_frame_none_is_noop() {
    V1Display display;
    display.renderFrame(RenderFrame{});

    TEST_ASSERT_EQUAL(0, g_idleCalls);
    TEST_ASSERT_EQUAL(0, g_liveCalls);
    TEST_ASSERT_EQUAL(0, g_persistedCalls);
}

void test_render_frame_idle_calls_resting_path() {
    V1Display display;
    RenderFrame frame{};
    frame.primaryKind = RenderFramePrimaryKind::IDLE;
    frame.primaryState = makeState();

    display.renderFrame(frame);

    TEST_ASSERT_EQUAL(1, g_idleCalls);
    TEST_ASSERT_EQUAL(0, g_liveCalls);
    TEST_ASSERT_EQUAL(0, g_persistedCalls);
}

void test_render_frame_v1_live_calls_live_path() {
    V1Display display;
    RenderFrame frame{};
    frame.primaryKind = RenderFramePrimaryKind::V1_LIVE;
    frame.v1Priority = makeAlert(BAND_KA, DIR_FRONT, 34520);
    frame.primaryState = makeState();
    frame.cardCount = 1;
    frame.cards[0].kind = RenderFrameCard::Kind::V1;
    frame.cards[0].v1Alert = makeAlert(BAND_K, DIR_REAR, 24148);

    display.renderFrame(frame);

    TEST_ASSERT_EQUAL(1, g_liveCalls);
    TEST_ASSERT_EQUAL(BAND_KA, g_lastPriority.band);
    // Card-row input contract: priority itself leads the alert list, cards
    // follow — so one card yields a two-entry list, never a null pointer.
    TEST_ASSERT_EQUAL(2, g_lastAlertCount);
    TEST_ASSERT_FALSE(g_lastAlertsWasNull);
    TEST_ASSERT_EQUAL(BAND_KA, g_lastFirstAlert.band);
    TEST_ASSERT_EQUAL_UINT32(34520u, g_lastFirstAlert.frequency);

    DisplayCorrectnessTraceEvent recent{};
    TEST_ASSERT_EQUAL_UINT32(1u, displayCorrectnessTraceCopyRecent(&recent, 1));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(DisplayCorrectnessOwner::V1),
                            static_cast<uint8_t>(recent.owner));
    TEST_ASSERT_EQUAL_UINT8(BAND_KA, recent.renderedBand);
    TEST_ASSERT_EQUAL_UINT32(34520u, recent.renderedFrequency);
}

void test_render_frame_alp_live_synthesizes_laser_priority() {
    V1Display display;
    RenderFrame frame{};
    frame.primaryKind = RenderFramePrimaryKind::ALP_LIVE;
    frame.alpPrimary.active = true;
    frame.alpPrimary.direction = AlpLaserDirection::REAR;
    frame.primaryState = makeState();
    frame.cardCount = 1;
    frame.cards[0].kind = RenderFrameCard::Kind::V1;
    frame.cards[0].v1Alert = makeAlert(BAND_KA, DIR_FRONT, 34520);

    display.renderFrame(frame);

    TEST_ASSERT_EQUAL(1, g_liveCalls);
    TEST_ASSERT_EQUAL(BAND_LASER, g_lastPriority.band);
    TEST_ASSERT_EQUAL(DIR_REAR, g_lastPriority.direction);
    // ALP frames carry the full unstripped V1 list in frame.cards — passed
    // through as-is (no synthetic-priority prepend), pointer non-null.
    TEST_ASSERT_EQUAL(1, g_lastAlertCount);
    TEST_ASSERT_FALSE(g_lastAlertsWasNull);
}

void test_render_frame_v1_live_zero_cards_passes_priority_only_list() {
    V1Display display;
    RenderFrame frame{};
    frame.primaryKind = RenderFramePrimaryKind::V1_LIVE;
    frame.v1Priority = makeAlert(BAND_KA, DIR_FRONT, 34520);
    frame.primaryState = makeState();
    frame.cardCount = 0;

    display.renderFrame(frame);

    TEST_ASSERT_EQUAL(1, g_liveCalls);
    // Zero secondaries during live is NOT the null clear-all signal: the
    // list still contains the priority so card grace windows keep aging.
    TEST_ASSERT_EQUAL(1, g_lastAlertCount);
    TEST_ASSERT_FALSE(g_lastAlertsWasNull);
    TEST_ASSERT_EQUAL(BAND_KA, g_lastFirstAlert.band);
}

void test_render_frame_v1_persisted_calls_persisted_path() {
    V1Display display;
    RenderFrame frame{};
    frame.primaryKind = RenderFramePrimaryKind::V1_PERSISTED;
    frame.v1Priority = makeAlert(BAND_KA, DIR_FRONT, 34520);
    frame.primaryState = makeState();

    display.renderFrame(frame);

    TEST_ASSERT_EQUAL(0, g_idleCalls);
    TEST_ASSERT_EQUAL(0, g_liveCalls);
    TEST_ASSERT_EQUAL(1, g_persistedCalls);
    TEST_ASSERT_EQUAL(BAND_KA, g_lastPriority.band);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_render_frame_none_is_noop);
    RUN_TEST(test_render_frame_idle_calls_resting_path);
    RUN_TEST(test_render_frame_v1_live_calls_live_path);
    RUN_TEST(test_render_frame_alp_live_synthesizes_laser_priority);
    RUN_TEST(test_render_frame_v1_live_zero_cards_passes_priority_only_list);
    RUN_TEST(test_render_frame_v1_persisted_calls_persisted_path);
    return UNITY_END();
}
