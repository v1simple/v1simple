// These native rendering tests cover card cache clearing and prevent a live
// priority alert from being re-admitted as a ghost secondary card.

#ifndef DISPLAY_WAVESHARE_349
#define DISPLAY_WAVESHARE_349 1
#endif

#include <unity.h>

#include <cstdio>

#include "../mocks/display_driver.h"
#include "../mocks/Arduino.h"
#include "../mocks/settings.h"
#include "../../src/packet_parser.h"
#include "../../include/band_utils.h"

#ifndef ARDUINO
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
SerialClass Serial;
#endif

#include "../../src/display.h"

V1Display* g_displayInstance = nullptr;
SettingsManager settings;
int bandIndicatorDrawCount = 0;
uint8_t lastBandIndicatorMask = 0;
bool lastBandIndicatorMuted = false;
int gpsIndicatorDrawCount = 0;
int bandIndicatorDrawOrder = 0;
int gpsIndicatorDrawOrder = 0;
int drawOrder = 0;

V1Display::V1Display(SettingsManager& injectedSettings) : settings_(injectedSettings) {
    currentPalette_ = ColorThemes::STANDARD();
    currentPalette_.colorMuted = settings_.get().colorMuted;
    currentPalette_.colorPersisted = settings_.get().colorPersisted;
    g_displayInstance = this;
}
V1Display::~V1Display() = default;

void V1Display::setPreviewIndicatorOverridesActive(bool active) {
    previewIndicatorOverridesActive_ = active;
}

const char* V1Display::bandToString(Band band) {
    return bandName(band);
}

uint16_t V1Display::getBandColor(Band band) {
    const V1Settings& s = settings.get();
    switch (band) {
        case BAND_LASER: return s.colorBandL;
        case BAND_KA: return s.colorBandKa;
        case BAND_K: return s.colorBandK;
        case BAND_KU: return s.colorBandK;
        case BAND_X: return s.colorBandX;
        default: return currentPalette_.text;
    }
}

bool V1Display::drawBandIndicators(uint8_t bandMask, bool muted, uint8_t) {
    bandIndicatorDrawCount++;
    bandIndicatorDrawOrder = ++drawOrder;
    lastBandIndicatorMask = bandMask;
    lastBandIndicatorMuted = muted;
    elementCaches_.bands.lastMask = bandMask;
    elementCaches_.bands.lastMuted = muted;
    elementCaches_.bands.lastPaletteRevision = paletteRevision_;
    elementCaches_.bands.valid = true;
    dirty_.gpsIndicator = true;
    return true;
}

void V1Display::drawGpsIndicator() {
    gpsIndicatorDrawCount++;
    gpsIndicatorDrawOrder = ++drawOrder;
    dirty_.gpsIndicator = false;
}

#include "../../src/display_cards.cpp"
#include "../../src/display_top_counter.cpp"
#include "../../src/display_frequency.cpp"
#include "../../src/display_frequency_digit_atlas.cpp"
#include "../../src/display_frequency_raster_cache.cpp"
#include "../../src/packet_parser.cpp"
#include "../../src/packet_parser_alerts.cpp"

// Top-counter font priming is outside these frequency/card tests.
bool DisplayFontManager::getTopCounterBounds(char, bool, int&, int&) { return false; }

V1Display display(settings);

namespace {

Arduino_Canvas* canvas() {
    return display.testCanvas();
}

void resetDisplayForTest() {
    display.~V1Display();
    new (&display) V1Display(settings);
    display.setTestCanvas(new Arduino_Canvas(SCREEN_WIDTH, SCREEN_HEIGHT, nullptr));
    display.ut_elementCaches() = DisplayElementCaches{};
    display.ut_resetDrawnRegion();
    canvas()->resetCounters();
    mockMillis = 1000;
    mockMicros = 1000;
    bandIndicatorDrawCount = 0;
    lastBandIndicatorMask = 0;
    lastBandIndicatorMuted = false;
    gpsIndicatorDrawCount = 0;
    bandIndicatorDrawOrder = 0;
    gpsIndicatorDrawOrder = 0;
    drawOrder = 0;
}

AlertData cardAlert() {
    return AlertData::create(BAND_KA, DIR_FRONT, 4, 0, 34700, true, true);
}


// Count how many of a card's meter segments were painted lit vs left as an
// outline. The renderer fills a lit segment and outlines an unlit one, so the
// two are distinguishable by which call the mock recorded for that rect.
struct CardMeterPaint {
    int lit = 0;
    int unlit = 0;
};

CardMeterPaint cardMeterPaint(int slot) {
    CardMeterPaint paint;
    for (int seg = 0; seg < DisplayLayout::CARD_METER_BAR_COUNT; ++seg) {
        const DisplayLayout::DisplayRect bar = DisplayLayout::cardMeterBarRect(slot, seg);
        for (const auto& call : canvas()->fillRectCalls) {
            if (call.x == bar.x && call.y == bar.y && call.w == bar.w && call.h == bar.h) {
                paint.lit++;
                break;
            }
        }
        for (const auto& call : canvas()->drawRectCalls) {
            if (call.x == bar.x && call.y == bar.y && call.w == bar.w && call.h == bar.h) {
                paint.unlit++;
                break;
            }
        }
    }
    return paint;
}

// Draw one secondary card at an exact signalled strength and report what its
// meter painted.
CardMeterPaint paintCardAtStrength(uint8_t bars) {
    resetDisplayForTest();
    AlertData priority = AlertData::create(BAND_X, DIR_REAR, 1, 0, 10525, true, true);
    AlertData secondary = AlertData::create(BAND_KA, DIR_FRONT, bars, 0, 34700, true, false);
    AlertData alerts[2] = {priority, secondary};
    display.ut_drawSecondaryAlertCards(alerts, 2, priority, false);
    return cardMeterPaint(0);
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
    cards.lastProfileSlot = settings.get().activeSlot;
    cards.lastDrawnPositions[0].band = BAND_KA;
    cards.lastDrawnPositions[0].frequency = 34700;
    cards.lastDrawnPositions[0].direction = DIR_FRONT;
    cards.lastDrawnPositions[0].bars = 4;
    cards.slots[0].alert = cardAlert();
    cards.slots[0].lastSeen = mockMillis;
    cards.lastPriority = cardAlert();
    auto& bands = display.ut_elementCaches().bands;
    bands.lastMask = static_cast<uint8_t>(BAND_K | BAND_KU);
    bands.lastMuted = true;
    bands.valid = true;

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
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, bandIndicatorDrawCount,
        "whole-row card clear must restore an exposed Ku label in the same frame");
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(BAND_K | BAND_KU), lastBandIndicatorMask);
    TEST_ASSERT_TRUE(lastBandIndicatorMuted);
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
    settings.slotAlertPersistSec[0] = 2;  // grace so a stale copy would persist

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

    settings.slotAlertPersistSec[0] = 0;
}

void test_secondary_frequency_drift_repaints_text_despite_bar_updates() {
    class TextCanvas : public Arduino_Canvas {
      public:
        TextCanvas() : Arduino_Canvas(SCREEN_WIDTH, SCREEN_HEIGHT, nullptr) {}
        std::string frequency;
        void print(const char* text) override {
            if (std::strncmp(text, "24.", 3) == 0) {
                frequency = text;
            }
        }
    };
    for (bool changeBars : {false, true}) {
        resetDisplayForTest();
        auto* text = new TextCanvas;
        display.setTestCanvas(text);
        AlertData priority = cardAlert();
        for (uint32_t step = 0; step <= 10; ++step) {
            // Each 3 MHz step is one continuing bogey. The visible text must
            // catch up once cumulative drift exceeds its 5 MHz hysteresis,
            // including when intervening frames repaint only the meter.
            AlertData secondary = AlertData::create(BAND_K, DIR_SIDE,
                changeBars && step % 2 ? 4 : 2, 0, 24150 + step * 3, true, false);
            AlertData alerts[] = {priority, secondary};
            mockMillis += 100;
            display.ut_drawSecondaryAlertCards(alerts, 2, priority, false);

            char expected[10];
            std::snprintf(expected, sizeof(expected), "24.%03u", 150 + (step / 2) * 6);
            TEST_ASSERT_EQUAL_STRING(expected, text->frequency.c_str());
            TEST_ASSERT_EQUAL_INT(1, display.ut_elementCaches().cards.lastDrawnCount);
        }
    }
}

// A vanished priority may become a card only when the user enabled persistence.
void test_priority_replacement_honors_zero_and_positive_persistence() {
    for (uint8_t persistSec : {0, 2}) {
        resetDisplayForTest();
        settings.slotAlertPersistSec[0] = persistSec;
        AlertData old = AlertData::create(BAND_KA, DIR_REAR, 4, 0, 35500, true, true);
        AlertData next = AlertData::create(BAND_K, DIR_SIDE, 2, 0, 24150, true, true);
        display.ut_drawSecondaryAlertCards(&old, 1, old, false);
        canvas()->resetCounters();
        display.ut_resetDrawnRegion();
        display.ut_drawSecondaryAlertCards(&next, 1, next, false);
        auto& cards = display.ut_elementCaches().cards;
        TEST_ASSERT_EQUAL_INT(persistSec == 0 ? 0 : 1, cards.lastDrawnCount);
        if (persistSec == 0) {
            TEST_ASSERT_TRUE_MESSAGE(display.ut_drawnRegionEmpty(),
                "zero persistence must not paint a vanished priority and schedule a later clear");
            TEST_ASSERT_EQUAL_UINT(0u, canvas()->fillRectCalls.size());
        } else {
            TEST_ASSERT_EQUAL_UINT32(35500, cards.lastDrawnPositions[0].frequency);
            TEST_ASSERT_TRUE(cards.lastDrawnPositions[0].isGraced);
            mockMillis += 2000;
            display.ut_drawSecondaryAlertCards(&next, 1, next, false);
            TEST_ASSERT_EQUAL_INT(1, cards.lastDrawnCount);
            ++mockMillis;
            display.ut_drawSecondaryAlertCards(&next, 1, next, false);
            TEST_ASSERT_EQUAL_INT(0, cards.lastDrawnCount);
        }
    }
    settings.slotAlertPersistSec[0] = 0;
}

void test_zero_persistence_clears_missing_secondary_in_same_millisecond() {
    for (uint8_t initialPersistSec : {0, 2}) {
        resetDisplayForTest();
        settings.slotAlertPersistSec[0] = initialPersistSec;
        AlertData p = AlertData::create(BAND_KA, DIR_FRONT, 4, 0, 34700, true, true);
        AlertData s = AlertData::create(BAND_K, DIR_SIDE, 2, 0, 24150, true, false);
        AlertData both[] = {p, s};
        display.ut_drawSecondaryAlertCards(both, 2, p, false);
        TEST_ASSERT_EQUAL_INT(1, display.ut_elementCaches().cards.lastDrawnCount);
        settings.slotAlertPersistSec[0] = 0;
        canvas()->resetCounters();
        display.ut_resetDrawnRegion();
        display.ut_drawSecondaryAlertCards(&p, 1, p, false);
        TEST_ASSERT_EQUAL_INT(0, display.ut_elementCaches().cards.lastDrawnCount);
        TEST_ASSERT_EQUAL_UINT8(BAND_NONE, display.ut_elementCaches().cards.lastDrawnPositions[0].band);
        TEST_ASSERT_GREATER_THAN_UINT(0u, canvas()->fillRectCalls.size());
        TEST_ASSERT_FALSE(display.ut_drawnRegionEmpty());
    }
}

void test_zero_persistence_releases_absent_slots_for_new_live_cards_immediately() {
    settings.slotAlertPersistSec[0] = 0;
    AlertData p = AlertData::create(BAND_KA, DIR_FRONT, 4, 0, 34700, true, true);
    AlertData old1 = AlertData::create(BAND_K, DIR_REAR, 2, 0, 24150, true, false);
    AlertData old2 = AlertData::create(BAND_X, DIR_SIDE, 3, 0, 10525, true, false);
    AlertData before[] = {p, old1, old2};
    display.ut_drawSecondaryAlertCards(before, 3, p, false);
    TEST_ASSERT_EQUAL_INT(2, display.ut_elementCaches().cards.lastDrawnCount);
    AlertData next1 = AlertData::create(BAND_K, DIR_FRONT, 3, 0, 24170, true, false);
    AlertData next2 = AlertData::create(BAND_KA, DIR_REAR, 5, 0, 35500, true, false);
    AlertData after[] = {p, next1, next2};
    display.ut_drawSecondaryAlertCards(after, 3, p, false);
    const auto& cards = display.ut_elementCaches().cards;
    TEST_ASSERT_EQUAL_INT(2, cards.lastDrawnCount);
    TEST_ASSERT_EQUAL_UINT32(24170, cards.lastDrawnPositions[0].frequency);
    TEST_ASSERT_EQUAL_UINT32(35500, cards.lastDrawnPositions[1].frequency);
    TEST_ASSERT_FALSE(cards.lastDrawnPositions[0].isGraced);
    TEST_ASSERT_FALSE(cards.lastDrawnPositions[1].isGraced);
}

void test_live_cards_replace_persisted_cards_before_grace_expires() {
    for (int newCount : {1, 2}) {
        for (unsigned long elapsed : {0UL, 1UL}) {
            resetDisplayForTest();
            settings.slotAlertPersistSec[0] = 5;
            AlertData p = cardAlert();
            AlertData old1 = AlertData::create(BAND_K, DIR_REAR, 2, 0, 24150);
            AlertData old2 = AlertData::create(BAND_X, DIR_SIDE, 3, 0, 10525);
            AlertData before[] = {p, old1, old2};
            display.ut_drawSecondaryAlertCards(before, 3, p, false);
            AlertData next1 = AlertData::create(BAND_KA, DIR_REAR, 5, 0, 35500);
            AlertData next2 = AlertData::create(BAND_K, DIR_FRONT, 3, 0, 24170);
            AlertData after[] = {p, next1, next2};
            mockMillis += elapsed;
            display.ut_drawSecondaryAlertCards(after, 1 + newCount, p, false);
            const auto& cards = display.ut_elementCaches().cards;
            TEST_ASSERT_EQUAL_INT(2, cards.lastDrawnCount);
            TEST_ASSERT_EQUAL_UINT32(35500, cards.lastDrawnPositions[0].frequency);
            TEST_ASSERT_FALSE(cards.lastDrawnPositions[0].isGraced);
            TEST_ASSERT_EQUAL_UINT32(newCount == 2 ? 24170 : 10525,
                                     cards.lastDrawnPositions[1].frequency);
            TEST_ASSERT_EQUAL(newCount == 1, cards.lastDrawnPositions[1].isGraced);
        }
    }
}

void test_new_card_evicts_only_persisted_slot_and_keeps_jittering_live_slot() {
    for (int liveSlot : {0, 1}) {
        resetDisplayForTest();
        settings.slotAlertPersistSec[0] = 5;
        AlertData p = cardAlert();
        AlertData live = AlertData::create(BAND_K, DIR_REAR, 2, 0, 24150);
        AlertData stale = AlertData::create(BAND_X, DIR_SIDE, 3, 0, 10525);
        AlertData before[] = {p, liveSlot == 0 ? live : stale, liveSlot == 0 ? stale : live};
        display.ut_drawSecondaryAlertCards(before, 3, p, false);
        live.frequency += 3; // Continuity jitter, beyond the identity tolerance.
        AlertData next = AlertData::create(BAND_KA, DIR_REAR, 5, 0, 35500);
        AlertData after[] = {p, next, live}; // New arrival precedes the surviving card.
        display.ut_drawSecondaryAlertCards(after, 3, p, false);
        const auto& cards = display.ut_elementCaches().cards;
        TEST_ASSERT_EQUAL_INT(2, cards.lastDrawnCount);
        TEST_ASSERT_EQUAL_UINT32(24153, cards.slots[liveSlot].alert.frequency);
        TEST_ASSERT_EQUAL_UINT32(24150, cards.lastDrawnPositions[liveSlot].frequency); // Redraw hysteresis.
        TEST_ASSERT_EQUAL_UINT32(35500, cards.lastDrawnPositions[1 - liveSlot].frequency);
        TEST_ASSERT_FALSE(cards.lastDrawnPositions[0].isGraced);
        TEST_ASSERT_FALSE(cards.lastDrawnPositions[1].isGraced);
    }
}

// ALP supplies a synthetic laser priority and the full live radar list.
void test_synthetic_laser_keeps_live_radar_cards_and_is_not_graced_on_exit() {
    for (uint8_t persistSec : {0, 2}) {
        resetDisplayForTest();
        settings.slotAlertPersistSec[0] = persistSec;
        AlertData radar = AlertData::create(BAND_KA, DIR_FRONT, 4, 0, 34700, true, true);
        AlertData secondary = AlertData::create(BAND_K, DIR_REAR, 2, 0, 24150, true, false);
        AlertData laser = AlertData::create(BAND_LASER, DIR_FRONT, 8, 0, 0, true, true);
        AlertData radarRows[] = {radar, secondary};
        display.ut_drawSecondaryAlertCards(radarRows, 2, radar, false);
        ++mockMillis;
        display.ut_drawSecondaryAlertCards(radarRows, 2, laser, false);
        const auto& cards = display.ut_elementCaches().cards;
        TEST_ASSERT_EQUAL_INT(2, cards.lastDrawnCount);
        TEST_ASSERT_EQUAL_UINT32(24150, cards.lastDrawnPositions[0].frequency);
        TEST_ASSERT_EQUAL_UINT32(34700, cards.lastDrawnPositions[1].frequency);
        TEST_ASSERT_FALSE(cards.lastDrawnPositions[0].isGraced);
        TEST_ASSERT_FALSE(cards.lastDrawnPositions[1].isGraced);
        ++mockMillis;
        display.ut_drawSecondaryAlertCards(radarRows, 2, radar, false);
        TEST_ASSERT_EQUAL_INT(1, cards.lastDrawnCount);
        TEST_ASSERT_EQUAL_UINT32(24150, cards.lastDrawnPositions[0].frequency);
        TEST_ASSERT_FALSE(cards.lastDrawnPositions[0].isGraced);
        for (const CardSlot& slot : cards.slots) {
            TEST_ASSERT_FALSE(slot.lastSeen > 0 && slot.alert.band == BAND_LASER);
        }
    }
    settings.slotAlertPersistSec[0] = 0;
}

void test_promoted_secondary_releases_its_card_for_the_still_live_old_priority() {
    for (uint8_t persistSec : {0, 2}) {
        for (uint32_t promotionJitter : {0u, 4u}) {
            resetDisplayForTest();
            settings.slotAlertPersistSec[0] = persistSec;
            AlertData a = AlertData::create(BAND_KA, DIR_FRONT, 4, 0, 34700, true, true);
            AlertData b = AlertData::create(BAND_K, DIR_SIDE, 3, 0, 24150, true, false);
            AlertData c = AlertData::create(BAND_X, DIR_REAR, 2, 0, 10525, true, false);
            AlertData before[] = {a, b, c};
            display.ut_drawSecondaryAlertCards(before, 3, a, false);
            TEST_ASSERT_EQUAL_INT(2, display.ut_elementCaches().cards.lastDrawnCount);

            a.isPriority = false;
            b.isPriority = true;
            b.frequency += promotionJitter;
            // renderFrame supplies the full live list, with the priority first.
            AlertData after[] = {b, a, c};
            mockMillis += 100;
            canvas()->resetCounters();
            display.ut_resetDrawnRegion();
            display.ut_drawSecondaryAlertCards(after, 3, b, false);
            auto& cards = display.ut_elementCaches().cards;
            TEST_ASSERT_EQUAL_INT(2, cards.lastDrawnCount);
            TEST_ASSERT_EQUAL_UINT32(34700, cards.lastDrawnPositions[0].frequency);
            TEST_ASSERT_EQUAL_UINT32(10525, cards.lastDrawnPositions[1].frequency);
            TEST_ASSERT_FALSE(cards.lastDrawnPositions[0].isGraced);
            TEST_ASSERT_GREATER_THAN_UINT(0u, canvas()->fillRectCalls.size());
            TEST_ASSERT_FALSE(display.ut_drawnRegionEmpty());

            for (int update = 0; update < 100; ++update) {
                mockMillis += 100;
                canvas()->resetCounters();
                display.ut_resetDrawnRegion();
                display.ut_drawSecondaryAlertCards(after, 3, b, false);
                TEST_ASSERT_EQUAL_INT(2, cards.lastDrawnCount);
                TEST_ASSERT_EQUAL_UINT32(34700, cards.lastDrawnPositions[0].frequency);
                TEST_ASSERT_EQUAL_UINT32(10525, cards.lastDrawnPositions[1].frequency);
                TEST_ASSERT_EQUAL_UINT(0u, canvas()->fillRectCalls.size());
                TEST_ASSERT_TRUE(display.ut_drawnRegionEmpty());
            }
        }
    }
    settings.slotAlertPersistSec[0] = 0;
}

void test_promoted_secondary_leaves_capacity_to_grace_the_vanished_old_priority() {
    settings.slotAlertPersistSec[0] = 2;
    AlertData a = AlertData::create(BAND_KA, DIR_FRONT, 4, 0, 34700, true, true);
    AlertData b = AlertData::create(BAND_K, DIR_SIDE, 3, 0, 24150, true, false);
    AlertData c = AlertData::create(BAND_X, DIR_REAR, 2, 0, 10525, true, false);
    AlertData before[] = {a, b, c};
    display.ut_drawSecondaryAlertCards(before, 3, a, false);
    b.isPriority = true;
    AlertData after[] = {b, c};
    mockMillis += 100;
    display.ut_drawSecondaryAlertCards(after, 2, b, false);
    auto& cards = display.ut_elementCaches().cards;
    TEST_ASSERT_EQUAL_INT(2, cards.lastDrawnCount);
    TEST_ASSERT_EQUAL_UINT32(34700, cards.lastDrawnPositions[0].frequency);
    TEST_ASSERT_TRUE(cards.lastDrawnPositions[0].isGraced);
    TEST_ASSERT_EQUAL_UINT32(10525, cards.lastDrawnPositions[1].frequency);

    mockMillis += 2001;
    canvas()->resetCounters();
    display.ut_resetDrawnRegion();
    display.ut_drawSecondaryAlertCards(after, 2, b, false);
    TEST_ASSERT_EQUAL_INT(1, cards.lastDrawnCount);
    TEST_ASSERT_EQUAL_UINT32(10525, cards.lastDrawnPositions[0].frequency);
    TEST_ASSERT_EQUAL_UINT8(BAND_NONE, cards.lastDrawnPositions[1].band);
    TEST_ASSERT_GREATER_THAN_UINT(0u, canvas()->fillRectCalls.size());
    TEST_ASSERT_FALSE(display.ut_drawnRegionEmpty());
    settings.slotAlertPersistSec[0] = 0;
}

void test_new_live_card_uses_promoted_slot_before_new_old_priority_grace() {
    settings.slotAlertPersistSec[0] = 2;
    AlertData a = AlertData::create(BAND_KA, DIR_FRONT, 4, 0, 34700, true, true);
    AlertData b = AlertData::create(BAND_K, DIR_SIDE, 3, 0, 24150, true, false);
    AlertData c = AlertData::create(BAND_X, DIR_REAR, 2, 0, 10525, true, false);
    AlertData d = AlertData::create(BAND_KA, DIR_REAR, 5, 0, 35500, true, false);
    AlertData before[] = {a, b, c};
    display.ut_drawSecondaryAlertCards(before, 3, a, false);
    b.isPriority = true;
    AlertData after[] = {b, c, d};
    mockMillis += 100;
    display.ut_drawSecondaryAlertCards(after, 3, b, false);
    auto& cards = display.ut_elementCaches().cards;
    TEST_ASSERT_EQUAL_INT(2, cards.lastDrawnCount);
    TEST_ASSERT_EQUAL_UINT32(35500, cards.lastDrawnPositions[0].frequency);
    TEST_ASSERT_FALSE(cards.lastDrawnPositions[0].isGraced);
    TEST_ASSERT_EQUAL_UINT32(10525, cards.lastDrawnPositions[1].frequency);
    settings.slotAlertPersistSec[0] = 0;
}

void test_preview_promotion_does_not_grace_the_vanished_old_priority() {
    settings.slotAlertPersistSec[0] = 2;
    AlertData a = AlertData::create(BAND_KA, DIR_FRONT, 4, 0, 34700, true, true);
    AlertData b = AlertData::create(BAND_K, DIR_SIDE, 3, 0, 24150, true, false);
    AlertData c = AlertData::create(BAND_X, DIR_REAR, 2, 0, 10525, true, false);
    AlertData before[] = {a, b, c};
    display.ut_drawSecondaryAlertCards(before, 3, a, false);
    b.isPriority = true;
    AlertData after[] = {b, c};
    display.setPreviewIndicatorOverridesActive(true);
    mockMillis += 100;
    display.ut_drawSecondaryAlertCards(after, 2, b, false);
    auto& cards = display.ut_elementCaches().cards;
    TEST_ASSERT_EQUAL_INT(1, cards.lastDrawnCount);
    TEST_ASSERT_EQUAL_UINT32(10525, cards.lastDrawnPositions[0].frequency);
    TEST_ASSERT_FALSE(cards.lastDrawnPositions[0].isGraced);
    settings.slotAlertPersistSec[0] = 0;
}

// A genuine secondary bogey whose signal ends must hold through the grace
// window (dimmed) and then clear its position with a repaint — while the
// priority stays live and leads the alert list every frame.
void test_secondary_card_expires_and_clears_after_grace_when_only_priority_remains() {
    settings.slotAlertPersistSec[0] = 2;  // 2 s grace window

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

    settings.slotAlertPersistSec[0] = 0;
}

void test_removing_card0_restores_exposed_ku_label_in_same_frame() {
    AlertData priority = AlertData::create(BAND_KA, DIR_FRONT, 4, 0, 34700, true, true);
    AlertData secondary = AlertData::create(BAND_K, DIR_FRONT, 3, 0, 24150, true, true);
    AlertData both[2] = {priority, secondary};
    display.ut_drawSecondaryAlertCards(both, 2, priority, false);
    TEST_ASSERT_EQUAL_INT(1, display.ut_elementCaches().cards.lastDrawnCount);

    auto& bands = display.ut_elementCaches().bands;
    bands.lastMask = static_cast<uint8_t>(BAND_K | BAND_KU);
    bands.lastMuted = false;
    bands.valid = true;
    auto& gps = display.ut_elementCaches().gps;
    gps.valid = true;
    gps.lastShown = true;
    gps.lastSats = 7;
    bandIndicatorDrawCount = 0;
    gpsIndicatorDrawCount = 0;
    drawOrder = 0;

    AlertData onlyPriority[1] = {priority};
    mockMillis += 100;
    display.ut_drawSecondaryAlertCards(onlyPriority, 1, priority, false);

    TEST_ASSERT_EQUAL_INT(0, display.ut_elementCaches().cards.lastDrawnCount);
    TEST_ASSERT_EQUAL_UINT8(BAND_NONE, display.ut_elementCaches().cards.lastDrawnPositions[0].band);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, bandIndicatorDrawCount,
        "card-0 removal must restore an exposed Ku label before the owning flush");
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(BAND_K | BAND_KU), lastBandIndicatorMask);
    TEST_ASSERT_FALSE(lastBandIndicatorMuted);
    TEST_ASSERT_TRUE(display.ut_elementCaches().bands.valid);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, gpsIndicatorDrawCount,
        "card-0 removal must repaint GPS after the nested full-stack band redraw");
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(bandIndicatorDrawOrder, gpsIndicatorDrawOrder,
        "GPS must be restored after bands in the same card-removal frame");
    TEST_ASSERT_TRUE(gps.valid);
    TEST_ASSERT_TRUE(gps.lastShown);
    TEST_ASSERT_EQUAL_UINT8(7, gps.lastSats);
}

void test_visual_preview_bypasses_profile_card_grace() {
    settings.slotAlertPersistSec[0] = 5;

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
    settings.slotAlertPersistSec[0] = 0;
}


// Alert rows use VR's diagnostic 0..8 strength scale. The six-cell card meter
// preserves that ordering with the historical proportional projection.
void test_card_meter_projects_vr_strength_onto_six_segments() {
    static constexpr uint8_t expected[9] = {0, 1, 2, 2, 3, 4, 5, 5, 6};
    for (int bars = 0; bars <= 8; ++bars) {
        const CardMeterPaint paint = paintCardAtStrength(static_cast<uint8_t>(bars));
        char msg[96];
        std::snprintf(msg, sizeof(msg), "strength %d: lit=%d unlit=%d", bars, paint.lit, paint.unlit);
        TEST_ASSERT_EQUAL_INT_MESSAGE(expected[bars], paint.lit, msg);
        TEST_ASSERT_EQUAL_INT_MESSAGE(DisplayLayout::CARD_METER_BAR_COUNT - expected[bars], paint.unlit, msg);
    }
}

// Every segment must be accounted for at every step: nothing unpainted, and no
// segment both filled and outlined in the same frame.
void test_card_meter_paints_every_segment_exactly_once_per_frame() {
    for (int bars = 0; bars <= 8; ++bars) {
        const CardMeterPaint paint = paintCardAtStrength(static_cast<uint8_t>(bars));
        char msg[96];
        std::snprintf(msg, sizeof(msg), "strength %d covered %d of %d segments", bars,
                      paint.lit + paint.unlit, DisplayLayout::CARD_METER_BAR_COUNT);
        TEST_ASSERT_EQUAL_INT_MESSAGE(DisplayLayout::CARD_METER_BAR_COUNT, paint.lit + paint.unlit, msg);
    }
}

void test_card_meter_projection_is_monotonic_and_reaches_both_endpoints() {
    int previous = -1;
    for (int bars = 0; bars <= 8; ++bars) {
        const int lit = paintCardAtStrength(static_cast<uint8_t>(bars)).lit;
        if (previous >= 0) {
            char msg[96];
            std::snprintf(msg, sizeof(msg), "strength %d lit %d, previous step lit %d", bars, lit, previous);
            TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(previous, lit, msg);
        }
        previous = lit;
    }
    TEST_ASSERT_EQUAL_INT(0, paintCardAtStrength(0).lit);
    TEST_ASSERT_EQUAL_INT(6, paintCardAtStrength(8).lit);
}

// Strength above full scale saturates instead of overflowing the segment array.
void test_card_meter_clamps_strength_above_full_scale() {
    const CardMeterPaint paint = paintCardAtStrength(11);
    TEST_ASSERT_EQUAL_INT(DisplayLayout::CARD_METER_BAR_COUNT, paint.lit);
    TEST_ASSERT_EQUAL_INT(0, paint.unlit);
}

// Lit segments take their own stored colour, with no interpolation at draw
// time -- the same rule the main meter follows.
void test_card_meter_lit_segments_use_their_own_stored_colors() {
    V1Settings& s = settings.mutableSettings();
    for (int seg = 0; seg < SIGNAL_BAR_COLOR_COUNT; ++seg) {
        s.colorBars[seg] = static_cast<uint16_t>(0x1000 + seg);
    }
    paintCardAtStrength(8);
    for (int seg = 0; seg < DisplayLayout::CARD_METER_BAR_COUNT; ++seg) {
        const DisplayLayout::DisplayRect bar = DisplayLayout::cardMeterBarRect(0, seg);
        bool found = false;
        for (const auto& call : canvas()->fillRectCalls) {
            if (call.x == bar.x && call.y == bar.y && call.w == bar.w && call.h == bar.h) {
                char msg[64];
                std::snprintf(msg, sizeof(msg), "segment %d colour", seg);
                TEST_ASSERT_EQUAL_UINT16_MESSAGE(s.colorBars[seg], call.color, msg);
                found = true;
                break;
            }
        }
        TEST_ASSERT_TRUE_MESSAGE(found, "every segment must be painted at full scale");
    }
}

void test_parsed_ku_secondary_uses_production_band_name_and_frequency() {
    class TextCanvas : public Arduino_Canvas {
      public:
        TextCanvas() : Arduino_Canvas(SCREEN_WIDTH, SCREEN_HEIGHT, nullptr) {}
        std::vector<std::string> labels;
        void print(const char* text) override { labels.emplace_back(text); }
    };
    PacketParser parser;
    auto addRow = [&parser](uint8_t index, uint16_t frequency, uint8_t bandAndDirection, bool priority) {
        std::vector<uint8_t> packet = {
            0xAA, 0xDA, 0xE4, 0x43, 9, static_cast<uint8_t>((index << 4) | 2),
            static_cast<uint8_t>(frequency >> 8), static_cast<uint8_t>(frequency),
            0xA0, 0, bandAndDirection, static_cast<uint8_t>(priority ? 0x80 : 0), 0};
        uint8_t checksum = 0;
        for (uint8_t byte : packet) {
            checksum += byte;
        }
        packet.push_back(checksum);
        packet.push_back(0xAB);
        TEST_ASSERT_TRUE(parser.parse(packet.data(), packet.size(), 1000));
    };
    addRow(1, 34700, 0x22, true);
    addRow(2, 13450, 0x90, false);
    TEST_ASSERT_EQUAL_UINT(2, parser.getAlertCount());
    TEST_ASSERT_EQUAL_INT(BAND_KU, parser.getAllAlerts()[1].band);
    auto* text = new TextCanvas;
    display.setTestCanvas(text);
    display.ut_drawSecondaryAlertCards(parser.getAllAlerts().data(), parser.getAlertCount(),
                                       parser.getPriorityAlert(), false);
    TEST_ASSERT_EQUAL_INT(1, display.ut_elementCaches().cards.lastDrawnCount);
    TEST_ASSERT_EQUAL_UINT(2, text->labels.size());
    TEST_ASSERT_EQUAL_STRING("Ku", text->labels[0].c_str());
    TEST_ASSERT_EQUAL_STRING("13.450", text->labels[1].c_str());
}

// Rasterize the straight border and segment interiors needed by these tests.
// This checks shared framebuffer ownership, not panel or font raster fidelity.
class FrequencyCardCanvas : public Arduino_Canvas {
  public:
    FrequencyCardCanvas() : Arduino_Canvas(SCREEN_WIDTH, SCREEN_HEIGHT, nullptr) {}
    void drawRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint16_t color) override {
        Arduino_Canvas::fillRect(x + r, y, w - 2 * r, 1, color);
        Arduino_Canvas::fillRect(x + r, y + h - 1, w - 2 * r, 1, color);
    }
    void fillRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint16_t color) override {
        Arduino_Canvas::fillRoundRect(x, y, w, h, r, color);
        Arduino_Canvas::fillRect(x + r, y, w - 2 * r, h, color);
    }
    void fillCircle(int16_t x, int16_t y, int16_t r, uint16_t color) override {
        TEST_ASSERT_LESS_THAN_INT(DisplayLayout::CONTENT_BOTTOM_Y, y + r);
        Arduino_Canvas::fillCircle(x, y, r, color);
    }
    uint16_t pixel(int x, int y) { return getFramebuffer()[x * CANVAS_WIDTH + CANVAS_WIDTH - 1 - y]; }
};

void test_frequency_updates_preserve_cached_card_border() {
    for (bool fontReady : {false, true}) {
        resetDisplayForTest();
        auto* pixels = new FrequencyCardCanvas;
        display.setTestCanvas(pixels);
        display.ut_fontMgr().segment7Ready = fontReady;
        AlertData primary = AlertData::create(BAND_KA, DIR_FRONT, 4, 0, 34700, true, true);
        const AlertData secondary = AlertData::create(BAND_X, DIR_REAR, 0, 2, 10525, true, false);
        AlertData alerts[2] = {primary, secondary};
        display.ut_drawFrequency(primary.frequency, primary.band);
        display.ut_drawSecondaryAlertCards(alerts, 2, primary, false);
        const auto card = DisplayLayout::cardRect(0);
        std::vector<uint16_t> before;
        for (int x = card.x + 5; x < card.x + card.w - 5; ++x) {
            TEST_ASSERT_NOT_EQUAL(0, pixels->pixel(x, card.y));
            before.push_back(pixels->pixel(x, card.y));
        }
        pixels->resetCounters();
        mockMillis += 100;
        primary.frequency = alerts[0].frequency = 34701;
        display.ut_drawFrequency(primary.frequency, primary.band);
        display.ut_drawSecondaryAlertCards(alerts, 2, primary, false);
        for (int x = card.x + 5; x < card.x + card.w - 5; ++x) {
            TEST_ASSERT_EQUAL_UINT16(before[x - card.x - 5], pixels->pixel(x, card.y));
        }
        for (const auto& call : pixels->fillRoundRectCalls) {
            TEST_ASSERT_FALSE(call.x == card.x && call.y == card.y && call.w == card.w && call.h == card.h);
        }
        pixels->resetCounters();
        display.ut_drawFrequency(primary.frequency, primary.band);
        TEST_ASSERT_TRUE(pixels->fillRectCalls.empty());
        TEST_ASSERT_TRUE(pixels->fillRoundRectCalls.empty());
    }
}

void test_fallback_shorter_text_clears_previous_text_extent() {
    auto* pixels = new FrequencyCardCanvas;
    display.setTestCanvas(pixels);
    display.ut_fontMgr().segment7Ready = false;
    display.ut_drawFrequency(0, BAND_LASER);
    display.ut_drawFrequency(0, BAND_LASER, "LTI");
    const std::vector<uint16_t> after(pixels->getFramebuffer(),
                                     pixels->getFramebuffer() + CANVAS_WIDTH * CANVAS_HEIGHT);
    resetDisplayForTest();
    pixels = new FrequencyCardCanvas;
    display.setTestCanvas(pixels);
    display.ut_drawFrequency(0, BAND_LASER, "LTI");
    TEST_ASSERT_EQUAL_UINT16_ARRAY(pixels->getFramebuffer(), after.data(), after.size());
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_empty_card_clear_is_noop_when_no_cards_were_drawn);
    RUN_TEST(test_card_clear_repaints_and_resets_previous_drawn_card_state);
    RUN_TEST(test_priority_frequency_jitter_does_not_admit_ghost_card);
    RUN_TEST(test_secondary_frequency_jitter_refreshes_slot_without_duplicate);
    RUN_TEST(test_secondary_frequency_drift_repaints_text_despite_bar_updates);
    RUN_TEST(test_priority_replacement_honors_zero_and_positive_persistence);
    RUN_TEST(test_zero_persistence_clears_missing_secondary_in_same_millisecond);
    RUN_TEST(test_zero_persistence_releases_absent_slots_for_new_live_cards_immediately);
    RUN_TEST(test_live_cards_replace_persisted_cards_before_grace_expires);
    RUN_TEST(test_new_card_evicts_only_persisted_slot_and_keeps_jittering_live_slot);
    RUN_TEST(test_synthetic_laser_keeps_live_radar_cards_and_is_not_graced_on_exit);
    RUN_TEST(test_promoted_secondary_releases_its_card_for_the_still_live_old_priority);
    RUN_TEST(test_promoted_secondary_leaves_capacity_to_grace_the_vanished_old_priority);
    RUN_TEST(test_new_live_card_uses_promoted_slot_before_new_old_priority_grace);
    RUN_TEST(test_preview_promotion_does_not_grace_the_vanished_old_priority);
    RUN_TEST(test_secondary_card_expires_and_clears_after_grace_when_only_priority_remains);
    RUN_TEST(test_removing_card0_restores_exposed_ku_label_in_same_frame);
    RUN_TEST(test_visual_preview_bypasses_profile_card_grace);
    RUN_TEST(test_card_meter_projects_vr_strength_onto_six_segments);
    RUN_TEST(test_card_meter_paints_every_segment_exactly_once_per_frame);
    RUN_TEST(test_card_meter_projection_is_monotonic_and_reaches_both_endpoints);
    RUN_TEST(test_card_meter_clamps_strength_above_full_scale);
    RUN_TEST(test_card_meter_lit_segments_use_their_own_stored_colors);
    RUN_TEST(test_parsed_ku_secondary_uses_production_band_name_and_frequency);
    RUN_TEST(test_frequency_updates_preserve_cached_card_border);
    RUN_TEST(test_fallback_shorter_text_clears_previous_text_extent);
    return UNITY_END();
}
