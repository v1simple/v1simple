#include <unity.h>
#include <cstdint>
#include <cmath>

// ── Minimal stub for ObdRuntimeModule ────────────────────────────
// Provides getFreshSpeed() for SpeedSourceSelector to call.

class ObdRuntimeModule {
public:
    bool freshResult = false;
    float freshSpeedMph = 0.0f;
    uint32_t freshTsMs = 0;

    bool getFreshSpeed(uint32_t /*nowMs*/, float& speedMphOut, uint32_t& tsMsOut) const {
        if (!freshResult) return false;
        speedMphOut = freshSpeedMph;
        tsMsOut = freshTsMs;
        return true;
    }
};

ObdRuntimeModule obdRuntimeModule;

// ── Minimal stub for GpsRuntimeModule ────────────────────────────
// Mirrors the production surface needed by SpeedSourceSelector:
//   - bool getFreshSpeed(nowMs, speedMphOut, tsMsOut)
//   - GpsRuntimeStatus snapshot(nowMs)
#include "../../src/modules/gps/gps_runtime_status.h"

class GpsRuntimeModule {
public:
    bool freshResult = false;
    float freshSpeedMph = 0.0f;
    uint32_t freshTsMs = 0;
    GpsRuntimeStatus nextSnapshot{};

    bool getFreshSpeed(uint32_t /*nowMs*/, float& speedMphOut, uint32_t& tsMsOut) const {
        if (!freshResult) return false;
        speedMphOut = freshSpeedMph;
        tsMsOut = freshTsMs;
        return true;
    }

    GpsRuntimeStatus snapshot(uint32_t /*nowMs*/) const {
        return nextSnapshot;
    }
};

GpsRuntimeModule gpsRuntimeModule;

#include "../../src/modules/speed/speed_source_selector.h"
#include "../../src/modules/speed/speed_source_selector.cpp"

SpeedSourceSelector speedSourceSelector;

// Helper: mark GPS sample as "good signal" (fresh + stable fix + sats + HDOP).
static void setGpsGood(float speedMph, uint32_t tsMs) {
    gpsRuntimeModule.freshResult = true;
    gpsRuntimeModule.freshSpeedMph = speedMph;
    gpsRuntimeModule.freshTsMs = tsMs;
    gpsRuntimeModule.nextSnapshot = GpsRuntimeStatus{};
    gpsRuntimeModule.nextSnapshot.stableHasFix = true;
    gpsRuntimeModule.nextSnapshot.satellites = 8;
    gpsRuntimeModule.nextSnapshot.hdop = 1.5f;
    gpsRuntimeModule.nextSnapshot.speedMph = speedMph;
}

static void resetAll() {
    speedSourceSelector = SpeedSourceSelector();
    obdRuntimeModule = ObdRuntimeModule();
    gpsRuntimeModule = GpsRuntimeModule();
}

void setUp() { resetAll(); }
void tearDown() {}

static SpeedSelectorStatus snapshotAt(uint32_t nowMs) {
    return speedSourceSelector.snapshotAt(nowMs);
}

static SpeedSelectorStatus updateAndSnapshot(uint32_t nowMs) {
    speedSourceSelector.update(nowMs);
    return speedSourceSelector.snapshot();
}

// ── Enum value tests ──────────────────────────────────────────────

void test_obd_enum_value_is_3() {
    TEST_ASSERT_EQUAL(3, static_cast<uint8_t>(SpeedSource::OBD));
}

void test_gps_enum_value_is_1() {
    TEST_ASSERT_EQUAL(1, static_cast<uint8_t>(SpeedSource::GPS));
}

void test_source_name_obd() {
    TEST_ASSERT_EQUAL_STRING("obd", SpeedSourceSelector::sourceName(SpeedSource::OBD));
}

void test_source_name_gps() {
    TEST_ASSERT_EQUAL_STRING("gps", SpeedSourceSelector::sourceName(SpeedSource::GPS));
}

void test_source_name_none() {
    TEST_ASSERT_EQUAL_STRING("none", SpeedSourceSelector::sourceName(SpeedSource::NONE));
}

// ── No sources ────────────────────────────────────────────────────

void test_no_sources_enabled_selects_none() {
    speedSourceSelector.begin(&obdRuntimeModule, false, &gpsRuntimeModule, false);
    auto s = snapshotAt(1000);
    TEST_ASSERT_EQUAL(SpeedSource::NONE, s.selectedSource);
    TEST_ASSERT_FALSE(s.obdFresh);
    TEST_ASSERT_FALSE(s.gpsFresh);
}

void test_obd_enabled_no_data_selects_none() {
    speedSourceSelector.begin(&obdRuntimeModule, true, &gpsRuntimeModule, false);
    auto s = snapshotAt(1000);
    TEST_ASSERT_EQUAL(SpeedSource::NONE, s.selectedSource);
    TEST_ASSERT_FALSE(s.obdFresh);
}

// ── OBD only ──────────────────────────────────────────────────────

void test_obd_only_fresh_selects_obd() {
    speedSourceSelector.begin(&obdRuntimeModule, true, &gpsRuntimeModule, false);
    obdRuntimeModule.freshResult = true;
    obdRuntimeModule.freshSpeedMph = 72.0f;
    obdRuntimeModule.freshTsMs = 950;

    auto s = snapshotAt(1000);
    TEST_ASSERT_EQUAL(SpeedSource::OBD, s.selectedSource);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 72.0f, s.selectedSpeedMph);
    TEST_ASSERT_EQUAL(50, s.selectedAgeMs);
    TEST_ASSERT_TRUE(s.obdFresh);
}

void test_obd_disabled_not_polled() {
    speedSourceSelector.begin(&obdRuntimeModule, false, &gpsRuntimeModule, false);
    obdRuntimeModule.freshResult = true;
    obdRuntimeModule.freshSpeedMph = 72.0f;
    obdRuntimeModule.freshTsMs = 950;

    auto s = snapshotAt(1000);
    TEST_ASSERT_EQUAL(SpeedSource::NONE, s.selectedSource);
    TEST_ASSERT_FALSE(s.obdFresh);
}

// ── Speed validation (MAX_VALID_SPEED_MPH) ────────────────────────

void test_obd_over_max_speed_rejected() {
    speedSourceSelector.begin(&obdRuntimeModule, true, &gpsRuntimeModule, false);
    obdRuntimeModule.freshResult = true;
    obdRuntimeModule.freshSpeedMph = 260.0f;  // > 250
    obdRuntimeModule.freshTsMs = 900;

    auto s = snapshotAt(1000);
    TEST_ASSERT_EQUAL(SpeedSource::NONE, s.selectedSource);
    TEST_ASSERT_FALSE(s.obdFresh);
}

void test_speed_at_max_is_valid() {
    speedSourceSelector.begin(&obdRuntimeModule, true, &gpsRuntimeModule, false);
    obdRuntimeModule.freshResult = true;
    obdRuntimeModule.freshSpeedMph = 250.0f;  // == MAX
    obdRuntimeModule.freshTsMs = 900;

    auto s = snapshotAt(1000);
    TEST_ASSERT_EQUAL(SpeedSource::OBD, s.selectedSource);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 250.0f, s.selectedSpeedMph);
}

// ── Zero speed ────────────────────────────────────────────────────

void test_obd_zero_speed_is_valid() {
    speedSourceSelector.begin(&obdRuntimeModule, true, &gpsRuntimeModule, false);
    obdRuntimeModule.freshResult = true;
    obdRuntimeModule.freshSpeedMph = 0.0f;
    obdRuntimeModule.freshTsMs = 900;

    auto s = snapshotAt(1000);
    TEST_ASSERT_EQUAL(SpeedSource::OBD, s.selectedSource);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, s.selectedSpeedMph);
}

// ── Source switching counter ──────────────────────────────────────

void test_source_switch_counter_increments() {
    speedSourceSelector.begin(&obdRuntimeModule, true, &gpsRuntimeModule, false);

    obdRuntimeModule.freshResult = true;
    obdRuntimeModule.freshSpeedMph = 62.0f;
    obdRuntimeModule.freshTsMs = 900;
    auto s1 = updateAndSnapshot(1000);
    TEST_ASSERT_EQUAL(SpeedSource::OBD, s1.selectedSource);
    TEST_ASSERT_EQUAL(0, s1.sourceSwitches);

    obdRuntimeModule.freshResult = false;
    auto s2 = updateAndSnapshot(1500);
    TEST_ASSERT_EQUAL(SpeedSource::NONE, s2.selectedSource);
    TEST_ASSERT_EQUAL(1, s2.sourceSwitches);

    obdRuntimeModule.freshResult = true;
    obdRuntimeModule.freshTsMs = 1400;
    auto s3 = updateAndSnapshot(1500);
    TEST_ASSERT_EQUAL(SpeedSource::OBD, s3.selectedSource);
    TEST_ASSERT_EQUAL(1, s3.sourceSwitches);
}

void test_snapshot_at_is_pure_and_update_commits_state() {
    speedSourceSelector.begin(&obdRuntimeModule, true, &gpsRuntimeModule, false);
    obdRuntimeModule.freshResult = true;
    obdRuntimeModule.freshSpeedMph = 60.0f;
    obdRuntimeModule.freshTsMs = 900;

    const SpeedSelectorStatus preview = snapshotAt(1000);
    TEST_ASSERT_EQUAL(SpeedSource::OBD, preview.selectedSource);
    TEST_ASSERT_EQUAL(0u, preview.obdSelections);
    TEST_ASSERT_FALSE(speedSourceSelector.selectedSpeed().valid);

    const SpeedSelectorStatus cachedBeforeUpdate = speedSourceSelector.snapshot();
    TEST_ASSERT_EQUAL(SpeedSource::NONE, cachedBeforeUpdate.selectedSource);
    TEST_ASSERT_EQUAL(0u, cachedBeforeUpdate.obdSelections);

    const SpeedSelectorStatus committed = updateAndSnapshot(1000);
    TEST_ASSERT_EQUAL(SpeedSource::OBD, committed.selectedSource);
    TEST_ASSERT_EQUAL(1u, committed.obdSelections);
    TEST_ASSERT_TRUE(speedSourceSelector.selectedSpeed().valid);
    TEST_ASSERT_EQUAL(SpeedSource::OBD, speedSourceSelector.selectedSpeed().source);
}

void test_selection_counters() {
    speedSourceSelector.begin(&obdRuntimeModule, true, &gpsRuntimeModule, false);

    obdRuntimeModule.freshResult = true;
    obdRuntimeModule.freshSpeedMph = 62.0f;
    obdRuntimeModule.freshTsMs = 900;
    speedSourceSelector.update(1000);

    obdRuntimeModule.freshResult = false;
    auto s = updateAndSnapshot(2000);

    TEST_ASSERT_EQUAL(1, s.obdSelections);
    TEST_ASSERT_EQUAL(1, s.noSourceSelections);
}

// ── Enabled input sync ────────────────────────────────────────────

void test_sync_enabled_inputs_activates_obd() {
    speedSourceSelector.begin(&obdRuntimeModule, false, &gpsRuntimeModule, false);
    obdRuntimeModule.freshResult = true;
    obdRuntimeModule.freshSpeedMph = 55.0f;
    obdRuntimeModule.freshTsMs = 900;

    auto s1 = snapshotAt(1000);
    TEST_ASSERT_EQUAL(SpeedSource::NONE, s1.selectedSource);

    speedSourceSelector.syncEnabledInputs(true, false);
    auto s2 = snapshotAt(1000);
    TEST_ASSERT_EQUAL(SpeedSource::OBD, s2.selectedSource);
    TEST_ASSERT_TRUE(s2.obdEnabled);
}

// ── Status fields ─────────────────────────────────────────────────

void test_status_reports_enabled_flags() {
    speedSourceSelector.begin(&obdRuntimeModule, true, &gpsRuntimeModule, true);
    auto s = snapshotAt(1000);
    TEST_ASSERT_TRUE(s.obdEnabled);
    TEST_ASSERT_TRUE(s.gpsEnabled);
}

void test_age_calculation() {
    speedSourceSelector.begin(&obdRuntimeModule, true, &gpsRuntimeModule, false);
    obdRuntimeModule.freshResult = true;
    obdRuntimeModule.freshSpeedMph = 62.0f;
    obdRuntimeModule.freshTsMs = 800;

    auto s = snapshotAt(1000);
    TEST_ASSERT_EQUAL(200, s.obdAgeMs);
    TEST_ASSERT_EQUAL(200, s.selectedAgeMs);
}

// ── GPS arbitration ───────────────────────────────────────────────

void test_gps_only_good_signal_selects_gps() {
    speedSourceSelector.begin(&obdRuntimeModule, false, &gpsRuntimeModule, true);
    setGpsGood(45.0f, 950);

    auto s = snapshotAt(1000);
    TEST_ASSERT_EQUAL(SpeedSource::GPS, s.selectedSource);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 45.0f, s.selectedSpeedMph);
    TEST_ASSERT_EQUAL(50, s.selectedAgeMs);
    TEST_ASSERT_TRUE(s.gpsFresh);
    TEST_ASSERT_TRUE(s.gpsGoodSignal);
}

void test_gps_disabled_not_polled() {
    speedSourceSelector.begin(&obdRuntimeModule, false, &gpsRuntimeModule, false);
    setGpsGood(45.0f, 950);

    auto s = snapshotAt(1000);
    TEST_ASSERT_EQUAL(SpeedSource::NONE, s.selectedSource);
    TEST_ASSERT_FALSE(s.gpsFresh);
}

void test_gps_no_stable_fix_rejected() {
    speedSourceSelector.begin(&obdRuntimeModule, false, &gpsRuntimeModule, true);
    setGpsGood(45.0f, 950);
    gpsRuntimeModule.nextSnapshot.stableHasFix = false;

    auto s = snapshotAt(1000);
    TEST_ASSERT_EQUAL(SpeedSource::NONE, s.selectedSource);
    TEST_ASSERT_TRUE(s.gpsFresh);
    TEST_ASSERT_FALSE(s.gpsGoodSignal);
}

void test_gps_too_few_satellites_rejected() {
    speedSourceSelector.begin(&obdRuntimeModule, false, &gpsRuntimeModule, true);
    setGpsGood(45.0f, 950);
    gpsRuntimeModule.nextSnapshot.satellites = 3;  // < MIN_SATELLITES (4)

    auto s = snapshotAt(1000);
    TEST_ASSERT_EQUAL(SpeedSource::NONE, s.selectedSource);
    TEST_ASSERT_FALSE(s.gpsGoodSignal);
}

void test_gps_high_hdop_rejected() {
    speedSourceSelector.begin(&obdRuntimeModule, false, &gpsRuntimeModule, true);
    setGpsGood(45.0f, 950);
    gpsRuntimeModule.nextSnapshot.hdop = 8.0f;  // > MAX_HDOP (5.0)

    auto s = snapshotAt(1000);
    TEST_ASSERT_EQUAL(SpeedSource::NONE, s.selectedSource);
    TEST_ASSERT_FALSE(s.gpsGoodSignal);
}

void test_gps_over_max_speed_rejected() {
    speedSourceSelector.begin(&obdRuntimeModule, false, &gpsRuntimeModule, true);
    setGpsGood(260.0f, 950);

    auto s = snapshotAt(1000);
    TEST_ASSERT_EQUAL(SpeedSource::NONE, s.selectedSource);
    TEST_ASSERT_FALSE(s.gpsFresh);
}

// ── Priority: OBD always wins over GPS when present ──────────────

void test_obd_wins_when_both_present() {
    speedSourceSelector.begin(&obdRuntimeModule, true, &gpsRuntimeModule, true);
    obdRuntimeModule.freshResult = true;
    obdRuntimeModule.freshSpeedMph = 70.0f;
    obdRuntimeModule.freshTsMs = 950;
    setGpsGood(45.0f, 950);

    auto s = snapshotAt(1000);
    TEST_ASSERT_EQUAL(SpeedSource::OBD, s.selectedSource);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 70.0f, s.selectedSpeedMph);
    // Both upstream signals are still reported in status.
    TEST_ASSERT_TRUE(s.obdFresh);
    TEST_ASSERT_TRUE(s.gpsGoodSignal);
}

void test_gps_used_when_obd_stale() {
    speedSourceSelector.begin(&obdRuntimeModule, true, &gpsRuntimeModule, true);
    obdRuntimeModule.freshResult = false;  // OBD not present / stale
    setGpsGood(45.0f, 950);

    auto s = snapshotAt(1000);
    TEST_ASSERT_EQUAL(SpeedSource::GPS, s.selectedSource);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 45.0f, s.selectedSpeedMph);
}

void test_gps_starts_first_then_obd_takes_over() {
    speedSourceSelector.begin(&obdRuntimeModule, true, &gpsRuntimeModule, true);

    // GPS up first, OBD not yet connected.
    setGpsGood(45.0f, 950);
    auto s1 = updateAndSnapshot(1000);
    TEST_ASSERT_EQUAL(SpeedSource::GPS, s1.selectedSource);
    TEST_ASSERT_EQUAL(1u, s1.gpsSelections);

    // OBD comes online — must take over immediately.
    obdRuntimeModule.freshResult = true;
    obdRuntimeModule.freshSpeedMph = 65.0f;
    obdRuntimeModule.freshTsMs = 1450;
    setGpsGood(46.0f, 1450);
    auto s2 = updateAndSnapshot(1500);
    TEST_ASSERT_EQUAL(SpeedSource::OBD, s2.selectedSource);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 65.0f, s2.selectedSpeedMph);
    TEST_ASSERT_EQUAL(1u, s2.obdSelections);
    TEST_ASSERT_EQUAL(1u, s2.sourceSwitches);  // GPS→OBD switch counted
}

void test_obd_drop_falls_back_to_gps() {
    speedSourceSelector.begin(&obdRuntimeModule, true, &gpsRuntimeModule, true);

    obdRuntimeModule.freshResult = true;
    obdRuntimeModule.freshSpeedMph = 70.0f;
    obdRuntimeModule.freshTsMs = 950;
    setGpsGood(45.0f, 950);
    updateAndSnapshot(1000);  // OBD selected

    // OBD drops; GPS still good — fall back to GPS.
    obdRuntimeModule.freshResult = false;
    setGpsGood(46.0f, 1450);
    auto s = updateAndSnapshot(1500);
    TEST_ASSERT_EQUAL(SpeedSource::GPS, s.selectedSource);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 46.0f, s.selectedSpeedMph);
    TEST_ASSERT_EQUAL(1u, s.gpsSelections);
    TEST_ASSERT_EQUAL(1u, s.sourceSwitches);  // OBD→GPS counted
}

int main(int /*argc*/, char** /*argv*/) {
    UNITY_BEGIN();

    // Enum & names
    RUN_TEST(test_obd_enum_value_is_3);
    RUN_TEST(test_gps_enum_value_is_1);
    RUN_TEST(test_source_name_obd);
    RUN_TEST(test_source_name_gps);
    RUN_TEST(test_source_name_none);

    // No sources
    RUN_TEST(test_no_sources_enabled_selects_none);
    RUN_TEST(test_obd_enabled_no_data_selects_none);

    // OBD only
    RUN_TEST(test_obd_only_fresh_selects_obd);
    RUN_TEST(test_obd_disabled_not_polled);

    // Speed validation
    RUN_TEST(test_obd_over_max_speed_rejected);
    RUN_TEST(test_speed_at_max_is_valid);
    RUN_TEST(test_obd_zero_speed_is_valid);

    // Source switching
    RUN_TEST(test_source_switch_counter_increments);
    RUN_TEST(test_snapshot_at_is_pure_and_update_commits_state);
    RUN_TEST(test_selection_counters);

    // Enabled input sync
    RUN_TEST(test_sync_enabled_inputs_activates_obd);

    // Status fields
    RUN_TEST(test_status_reports_enabled_flags);
    RUN_TEST(test_age_calculation);

    // GPS arbitration
    RUN_TEST(test_gps_only_good_signal_selects_gps);
    RUN_TEST(test_gps_disabled_not_polled);
    RUN_TEST(test_gps_no_stable_fix_rejected);
    RUN_TEST(test_gps_too_few_satellites_rejected);
    RUN_TEST(test_gps_high_hdop_rejected);
    RUN_TEST(test_gps_over_max_speed_rejected);

    // Priority OBD > GPS
    RUN_TEST(test_obd_wins_when_both_present);
    RUN_TEST(test_gps_used_when_obd_stale);
    RUN_TEST(test_gps_starts_first_then_obd_takes_over);
    RUN_TEST(test_obd_drop_falls_back_to_gps);

    return UNITY_END();
}
