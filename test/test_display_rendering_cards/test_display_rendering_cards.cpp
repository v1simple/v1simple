// Native rendering tests for secondary alert card cache/clear behavior.

#ifndef DISPLAY_WAVESHARE_349
#define DISPLAY_WAVESHARE_349 1
#endif

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

#include "../../src/display.h"

V1Display* g_displayInstance = nullptr;
SettingsManager settingsManager;

V1Display::V1Display() {
    currentPalette_ = ColorThemes::STANDARD();
    currentPalette_.colorMuted = settingsManager.get().colorMuted;
    currentPalette_.colorPersisted = settingsManager.get().colorPersisted;
    g_displayInstance = this;
}
V1Display::~V1Display() = default;

void V1Display::setPreviewIndicatorOverridesActive(bool active) {
    previewIndicatorOverridesActive_ = active;
}

const char* V1Display::bandToString(Band band) {
    switch (band) {
        case BAND_LASER: return "LASER";
        case BAND_KA: return "Ka";
        case BAND_K: return "K";
        case BAND_KU: return "Ku";
        case BAND_X: return "X";
        default: return "";
    }
}

uint16_t V1Display::getBandColor(Band band) {
    const V1Settings& s = settingsManager.get();
    switch (band) {
        case BAND_LASER: return s.colorBandL;
        case BAND_KA: return s.colorBandKa;
        case BAND_K: return s.colorBandK;
        case BAND_KU: return s.colorBandK;
        case BAND_X: return s.colorBandX;
        default: return currentPalette_.text;
    }
}

#include "../../src/display_cards.cpp"

V1Display display;

namespace {

Arduino_Canvas* canvas() {
    return display.testCanvas();
}

void resetDisplayForTest() {
    display.~V1Display();
    new (&display) V1Display();
    display.setTestCanvas(new Arduino_Canvas(SCREEN_WIDTH, SCREEN_HEIGHT, nullptr));
    display.ut_elementCaches() = DisplayElementCaches{};
    display.ut_resetDrawnRegion();
    canvas()->resetCounters();
    mockMillis = 1000;
    mockMicros = 1000;
}

AlertData cardAlert() {
    return AlertData::create(BAND_KA, DIR_FRONT, 4, 0, 34700, true, true);
}

}  // namespace

void setUp() {
    resetDisplayForTest();
}

void tearDown() {}

void test_empty_card_clear_is_noop_when_no_cards_were_drawn() {
    AlertData emptyPriority;

    display.ut_drawSecondaryAlertCards(nullptr, 0, emptyPriority, false);

    TEST_ASSERT_EQUAL_UINT_MESSAGE(0u, canvas()->fillRectCalls.size(),
        "empty resting card clear must not repaint the already-empty card area");
    TEST_ASSERT_TRUE_MESSAGE(display.ut_drawnRegionEmpty(),
        "empty resting card clear must not mark drawnRegion_ and force a full flush");
    TEST_ASSERT_EQUAL_INT(0, display.ut_elementCaches().cards.lastDrawnCount);
}

void test_card_clear_repaints_and_resets_previous_drawn_card_state() {
    auto& cards = display.ut_elementCaches().cards;
    cards.lastDrawnCount = 1;
    cards.lastProfileSlot = settingsManager.get().activeSlot;
    cards.lastDrawnPositions[0].band = BAND_KA;
    cards.lastDrawnPositions[0].frequency = 34700;
    cards.lastDrawnPositions[0].direction = DIR_FRONT;
    cards.lastDrawnPositions[0].bars = 4;
    cards.slots[0].alert = cardAlert();
    cards.slots[0].lastSeen = mockMillis;
    cards.lastPriority = cardAlert();

    AlertData emptyPriority;
    display.ut_drawSecondaryAlertCards(nullptr, 0, emptyPriority, false);

    TEST_ASSERT_GREATER_THAN_UINT_MESSAGE(0u, canvas()->fillRectCalls.size(),
        "clearing a previously drawn card must repaint the card area");
    TEST_ASSERT_FALSE_MESSAGE(display.ut_drawnRegionEmpty(),
        "clearing a previously drawn card must mark drawnRegion_ for the owning flush");
    TEST_ASSERT_EQUAL_INT(0, cards.lastDrawnCount);
    TEST_ASSERT_EQUAL_UINT8(BAND_NONE, cards.lastDrawnPositions[0].band);
    TEST_ASSERT_EQUAL_UINT8(BAND_NONE, cards.lastDrawnPositions[1].band);
    TEST_ASSERT_EQUAL_UINT32(0u, cards.lastDrawnPositions[0].frequency);
    TEST_ASSERT_EQUAL_UINT32(0u, cards.slots[0].lastSeen);
    TEST_ASSERT_FALSE(cards.lastPriority.isValid);
}

// Regression: the composer feeds a live alert list where the priority leads.
// A frame-to-frame priority frequency jitter beyond alertsMatch's ±2 MHz must
// NOT be treated as a priority handoff — before the jitter guard, it admitted
// a ghost copy of the live priority into slot 0 that re-admitted after every
// grace expiry ("first card never clears").
void test_priority_frequency_jitter_does_not_admit_ghost_card() {
    AlertData p1 = AlertData::create(BAND_KA, DIR_FRONT, 4, 0, 34700, true, true);
    AlertData alerts1[1] = {p1};
    display.ut_drawSecondaryAlertCards(alerts1, 1, p1, false);
    TEST_ASSERT_EQUAL_INT(0, display.ut_elementCaches().cards.lastDrawnCount);

    mockMillis += 100;
    AlertData p2 = AlertData::create(BAND_KA, DIR_FRONT, 4, 0, 34704, true, true);  // +4 MHz jitter
    AlertData alerts2[1] = {p2};
    display.ut_drawSecondaryAlertCards(alerts2, 1, p2, false);

    auto& cards = display.ut_elementCaches().cards;
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, cards.slots[0].lastSeen,
        "priority jitter must not grace-persist a ghost copy of the live priority");
    TEST_ASSERT_EQUAL_UINT32(0u, cards.slots[1].lastSeen);
    TEST_ASSERT_EQUAL_INT(0, cards.lastDrawnCount);
}

// A non-priority bogey jittering beyond ±2 MHz between frames must refresh its
// existing card slot (loose continuity match), not duplicate: previously the
// stale copy grace-persisted in one slot while the jittered reading was
// admitted as a "new" bogey into the other — two cards for one source, able to
// evict a genuine third bogey's card.
void test_secondary_frequency_jitter_refreshes_slot_without_duplicate() {
    settingsManager.slotAlertPersistSec[0] = 2;  // grace so a stale copy would persist

    AlertData p = AlertData::create(BAND_KA, DIR_FRONT, 4, 0, 34700, true, true);
    AlertData b1 = AlertData::create(BAND_K, DIR_FRONT, 3, 0, 24150, true, true);
    AlertData frame1[2] = {p, b1};
    display.ut_drawSecondaryAlertCards(frame1, 2, p, false);
    TEST_ASSERT_EQUAL_INT(1, display.ut_elementCaches().cards.lastDrawnCount);

    mockMillis += 100;
    AlertData b2 = AlertData::create(BAND_K, DIR_FRONT, 3, 0, 24154, true, true);  // +4 MHz jitter
    AlertData frame2[2] = {p, b2};
    display.ut_drawSecondaryAlertCards(frame2, 2, p, false);

    auto& cards = display.ut_elementCaches().cards;
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, cards.lastDrawnCount,
        "a jittering secondary bogey must stay a single card");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(24154u, cards.slots[0].alert.frequency,
        "slot refresh must adopt the latest jittered reading");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, cards.slots[1].lastSeen,
        "the jittered reading must not be admitted as a second card");

    // A genuinely distinct nearby bogey (+6 MHz, outside the ±5 continuity
    // window) is a new identity and takes the second slot.
    mockMillis += 100;
    AlertData c = AlertData::create(BAND_K, DIR_FRONT, 3, 0, 24160, true, true);
    AlertData frame3[3] = {p, b2, c};
    display.ut_drawSecondaryAlertCards(frame3, 3, p, false);
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, cards.lastDrawnCount,
        "a bogey outside the continuity window is a distinct card");

    settingsManager.slotAlertPersistSec[0] = 0;
}

// A genuine secondary bogey whose signal ends must hold through the grace
// window (dimmed) and then clear its position with a repaint — while the
// priority stays live and leads the alert list every frame.
void test_secondary_card_expires_and_clears_after_grace_when_only_priority_remains() {
    settingsManager.slotAlertPersistSec[0] = 2;  // 2 s grace window

    AlertData p = AlertData::create(BAND_KA, DIR_FRONT, 4, 0, 34700, true, true);
    AlertData b = AlertData::create(BAND_K, DIR_FRONT, 3, 0, 24150, true, true);
    AlertData both[2] = {p, b};
    display.ut_drawSecondaryAlertCards(both, 2, p, false);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, display.ut_elementCaches().cards.lastDrawnCount,
        "secondary bogey should occupy one card while priority is filtered out");

    // B's signal ends; the list still carries the live priority.
    AlertData onlyP[1] = {p};
    mockMillis += 500;
    display.ut_drawSecondaryAlertCards(onlyP, 1, p, false);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, display.ut_elementCaches().cards.lastDrawnCount,
        "expired signal should hold as a graced card inside the grace window");

    mockMillis += 2600;  // past the 2 s grace window
    canvas()->resetCounters();
    display.ut_drawSecondaryAlertCards(onlyP, 1, p, false);

    auto& cards = display.ut_elementCaches().cards;
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, cards.lastDrawnCount,
        "graced card must expire after the grace window");
    TEST_ASSERT_EQUAL_UINT8(BAND_NONE, cards.lastDrawnPositions[0].band);
    TEST_ASSERT_GREATER_THAN_UINT_MESSAGE(0u, canvas()->fillRectCalls.size(),
        "expiring the last card must repaint (clear) its screen position");

    settingsManager.slotAlertPersistSec[0] = 0;
}

void test_visual_preview_bypasses_profile_card_grace() {
    settingsManager.slotAlertPersistSec[0] = 5;

    AlertData priority = AlertData::create(BAND_KA, DIR_FRONT, 4, 0, 34700, true, true);
    AlertData secondary = AlertData::create(BAND_K, DIR_SIDE, 3, 0, 24150, true, true);
    AlertData both[2] = {priority, secondary};
    display.ut_drawSecondaryAlertCards(both, 2, priority, false);
    TEST_ASSERT_EQUAL_INT(1, display.ut_elementCaches().cards.lastDrawnCount);

    display.setPreviewIndicatorOverridesActive(true);
    AlertData onlyPriority[1] = {priority};
    display.ut_drawSecondaryAlertCards(onlyPriority, 1, priority, false);

    TEST_ASSERT_EQUAL_INT_MESSAGE(
        0,
        display.ut_elementCaches().cards.lastDrawnCount,
        "visual preview transitions must match their manifest instead of profile grace settings");
    TEST_ASSERT_EQUAL_UINT32(0u, display.ut_elementCaches().cards.slots[0].lastSeen);

    display.setPreviewIndicatorOverridesActive(false);
    settingsManager.slotAlertPersistSec[0] = 0;
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_empty_card_clear_is_noop_when_no_cards_were_drawn);
    RUN_TEST(test_card_clear_repaints_and_resets_previous_drawn_card_state);
    RUN_TEST(test_priority_frequency_jitter_does_not_admit_ghost_card);
    RUN_TEST(test_secondary_frequency_jitter_refreshes_slot_without_duplicate);
    RUN_TEST(test_secondary_card_expires_and_clears_after_grace_when_only_priority_remains);
    RUN_TEST(test_visual_preview_bypasses_profile_card_grace);
    return UNITY_END();
}
