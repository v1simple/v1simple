/**
 * V1Display — ALP Laser Event (Phase 2) Unit Tests
 *
 * Exercises the real setAlpLaserEvent() implementation from
 * display_indicators.cpp rather than the pipeline mock surface.
 */

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
#include "../../src/perf_metrics.h"
#include "../../include/display_element_caches.h"

V1Display* g_displayInstance = nullptr;
SettingsManager settingsManager;

void perfRecordDisplayStatusPaint(PerfDisplayStatusPaint) {}

V1Display::V1Display() {
    currentPalette_ = ColorThemes::STANDARD();
    currentPalette_.colorMuted = settingsManager.get().colorMuted;
    currentPalette_.colorPersisted = settingsManager.get().colorPersisted;
    g_displayInstance = this;
}

V1Display::~V1Display() = default;

V1Display display;

#include "../../src/display_indicators.cpp"

void V1Display::drawBLEProxyIndicator() {}

ObdRuntimeModule obdRuntimeModule;
AlpRuntimeModule alpRuntimeModule;
GpsRuntimeModule gpsRuntimeModule;

ObdRuntimeStatus ObdRuntimeModule::snapshot(uint32_t /*nowMs*/) const {
    return ObdRuntimeStatus{};
}

AlpStatus AlpRuntimeModule::snapshot() const {
    return AlpStatus{};
}

GpsRuntimeStatus GpsRuntimeModule::snapshot(uint32_t /*nowMs*/) const {
    return GpsRuntimeStatus{};
}

const char* alpGunAbbrev(AlpGunType gun) {
    switch (gun) {
        case AlpGunType::MARKSMAN_ULTRALYTE: return "ULT";
        case AlpGunType::PL3_PROLITE: return "PL3";
        case AlpGunType::DRAGONEYE_COMPACT: return "DE";
        case AlpGunType::LASER_ATLANTA_PL2: return "PL2";
        case AlpGunType::LTI_TRUSPEED_LR: return "TSLR";
        default: return "LASER";
    }
}

void AlpRuntimeModule::logDisplayDecision(uint32_t, const char*, const char*) {}

void setUp() {
    mockMillis = 1000;
    display.ut_elementCaches() = DisplayElementCaches{};
    display.setAlpLaserEvent(AlpLaserEvent{});
}

void tearDown() {}

void test_set_alp_laser_event_populates_atomic_fields() {
    AlpLaserEvent ev{};
    ev.active = true;
    ev.gun = AlpGunType::MARKSMAN_ULTRALYTE;
    ev.direction = AlpLaserDirection::FRONT;
    ev.lidActive = true;
    ev.openedAtMs = 5000;

    display.setAlpLaserEvent(ev);

    TEST_ASSERT_TRUE(display.ut_alpHasLaserEvent());
    TEST_ASSERT_TRUE(display.ut_alpFreqOverride());
    TEST_ASSERT_EQUAL_STRING("ULT", display.ut_alpFreqText());
    TEST_ASSERT_TRUE(display.ut_alpLidActive());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(AlpLaserDirection::FRONT),
                            display.ut_alpLaserDirectionRaw());
}

void test_set_alp_laser_event_inactive_clears_atomic_fields() {
    AlpLaserEvent activeEv{};
    activeEv.active = true;
    activeEv.gun = AlpGunType::LASER_ATLANTA_PL2;
    activeEv.direction = AlpLaserDirection::REAR;
    activeEv.lidActive = true;
    display.setAlpLaserEvent(activeEv);

    AlpLaserEvent inactiveEv{};
    display.setAlpLaserEvent(inactiveEv);

    TEST_ASSERT_FALSE(display.ut_alpHasLaserEvent());
    TEST_ASSERT_FALSE(display.ut_alpFreqOverride());
    TEST_ASSERT_EQUAL_STRING("", display.ut_alpFreqText());
    TEST_ASSERT_FALSE(display.ut_alpLidActive());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(AlpLaserDirection::UNKNOWN),
                            display.ut_alpLaserDirectionRaw());
}

void test_set_alp_laser_event_inactive_with_gun_keeps_persisted_context() {
    AlpLaserEvent persistedEv{};
    persistedEv.active = false;
    persistedEv.gun = AlpGunType::LASER_ATLANTA_PL2;
    persistedEv.direction = AlpLaserDirection::REAR;
    persistedEv.lidActive = true;

    display.setAlpLaserEvent(persistedEv);

    TEST_ASSERT_FALSE(display.ut_alpHasLaserEvent());
    TEST_ASSERT_TRUE(display.ut_alpFreqOverride());
    TEST_ASSERT_EQUAL_STRING("PL2", display.ut_alpFreqText());
    TEST_ASSERT_TRUE(display.ut_alpLidActive());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(AlpLaserDirection::REAR),
                            display.ut_alpLaserDirectionRaw());
}

void test_set_alp_laser_event_same_event_keeps_caches_valid() {
    AlpLaserEvent ev{};
    ev.active = true;
    ev.gun = AlpGunType::PL3_PROLITE;
    ev.direction = AlpLaserDirection::FRONT;
    display.setAlpLaserEvent(ev);

    display.ut_elementCaches().alp.valid = true;
    display.ut_elementCaches().arrow.valid = true;
    display.ut_elementCaches().frequency.valid = true;

    display.setAlpLaserEvent(ev);

    TEST_ASSERT_TRUE(display.ut_elementCaches().alp.valid);
    TEST_ASSERT_TRUE(display.ut_elementCaches().arrow.valid);
    TEST_ASSERT_TRUE(display.ut_elementCaches().frequency.valid);
}

void test_set_alp_laser_event_gun_change_invalidates_caches() {
    AlpLaserEvent first{};
    first.active = true;
    first.gun = AlpGunType::PL3_PROLITE;
    first.direction = AlpLaserDirection::FRONT;
    display.setAlpLaserEvent(first);

    display.ut_elementCaches().alp.valid = true;
    display.ut_elementCaches().arrow.valid = true;
    display.ut_elementCaches().frequency.valid = true;

    AlpLaserEvent second{};
    second.active = true;
    second.gun = AlpGunType::MARKSMAN_ULTRALYTE;
    second.direction = AlpLaserDirection::FRONT;
    display.setAlpLaserEvent(second);

    TEST_ASSERT_FALSE(display.ut_elementCaches().alp.valid);
    TEST_ASSERT_FALSE(display.ut_elementCaches().arrow.valid);
    TEST_ASSERT_FALSE(display.ut_elementCaches().frequency.valid);
    TEST_ASSERT_EQUAL_STRING("ULT", display.ut_alpFreqText());
}

void test_set_alp_laser_event_live_unknown_gun_keeps_previous_gun_text() {
    AlpLaserEvent first{};
    first.active = true;
    first.gun = AlpGunType::PL3_PROLITE;
    first.direction = AlpLaserDirection::FRONT;
    first.lidActive = true;
    display.setAlpLaserEvent(first);

    AlpLaserEvent second{};
    second.active = true;
    second.gun = AlpGunType::UNKNOWN;
    second.direction = AlpLaserDirection::FRONT;
    second.lidActive = false;
    display.setAlpLaserEvent(second);

    TEST_ASSERT_TRUE(display.ut_alpHasLaserEvent());
    TEST_ASSERT_TRUE(display.ut_alpFreqOverride());
    TEST_ASSERT_EQUAL_STRING("PL3", display.ut_alpFreqText());
    TEST_ASSERT_FALSE(display.ut_alpLidActive());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(AlpLaserDirection::FRONT),
                            display.ut_alpLaserDirectionRaw());
}

// Regression — Phase 2 Risk #1: mid-frame overwrite.
// syncTopIndicators() and refreshAlpIndicator() must update ONLY badge-context
// fields (alpEnabled_, alpStateRaw_, alpHbByte1_). The five event-owned fields
// (alpHasLaserEvent_, alpLaserEvent_.direction, alpFreqOverride_, alpFreqText_,
// alpLidActive_) are written atomically by setAlpLaserEvent() and must not be
// clobbered by a mid-frame snapshot read. If this test ever fails, someone has
// reintroduced the pre-Phase-2 drift bug where syncTopIndicators projected its
// own ALP state and raced setAlpLaserEvent().
void test_sync_top_indicators_does_not_clobber_event_owned_fields() {
    display.setAlpRuntimeModule(&alpRuntimeModule);

    AlpLaserEvent ev{};
    ev.active = true;
    ev.gun = AlpGunType::MARKSMAN_ULTRALYTE;
    ev.direction = AlpLaserDirection::FRONT;
    ev.lidActive = true;
    display.setAlpLaserEvent(ev);

    // Sanity: event-owned fields are populated before the mid-frame sync.
    TEST_ASSERT_TRUE(display.ut_alpHasLaserEvent());
    TEST_ASSERT_TRUE(display.ut_alpFreqOverride());
    TEST_ASSERT_EQUAL_STRING("ULT", display.ut_alpFreqText());
    TEST_ASSERT_TRUE(display.ut_alpLidActive());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(AlpLaserDirection::FRONT),
                            display.ut_alpLaserDirectionRaw());

    // Mock AlpRuntimeModule::snapshot() returns AlpStatus{} — state=OFF,
    // hbByte1=0, hasLaserEvent=false. syncTopIndicators() runs the same
    // badge-context write path refreshAlpIndicator() does; if either ever
    // regresses and starts writing event-owned fields from the snapshot,
    // the asserts below will fail because every field would be reset.
    display.ut_syncTopIndicators(mockMillis);

    TEST_ASSERT_TRUE(display.ut_alpHasLaserEvent());
    TEST_ASSERT_TRUE(display.ut_alpFreqOverride());
    TEST_ASSERT_EQUAL_STRING("ULT", display.ut_alpFreqText());
    TEST_ASSERT_TRUE(display.ut_alpLidActive());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(AlpLaserDirection::FRONT),
                            display.ut_alpLaserDirectionRaw());

    display.setAlpRuntimeModule(nullptr);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_set_alp_laser_event_populates_atomic_fields);
    RUN_TEST(test_set_alp_laser_event_inactive_clears_atomic_fields);
    RUN_TEST(test_set_alp_laser_event_inactive_with_gun_keeps_persisted_context);
    RUN_TEST(test_set_alp_laser_event_same_event_keeps_caches_valid);
    RUN_TEST(test_set_alp_laser_event_gun_change_invalidates_caches);
    RUN_TEST(test_set_alp_laser_event_live_unknown_gun_keeps_previous_gun_text);
    RUN_TEST(test_sync_top_indicators_does_not_clobber_event_owned_fields);
    return UNITY_END();
}
