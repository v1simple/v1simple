#include <unity.h>

#include "../../src/modules/gps/gps_runtime_module.h"
#include "../../src/modules/gps/gps_publishers.cpp"   // Pull GPS publisher globals for UNIT_TEST.
#include "../../src/modules/gps/gps_runtime_module.cpp"  // Pull implementation for UNIT_TEST.

#ifndef ARDUINO
SerialClass Serial;
#endif

unsigned long mockMillis = 0;
unsigned long mockMicros = 0;

static void resetRuntime() {
    gpsRuntimeModule = GpsRuntimeModule();
    mockMillis = 1;
    mockMicros = 1000;
    gpsRuntimeModule.begin(true);
}

void setUp() {
    resetRuntime();
}

void tearDown() {
}

void test_valid_rmc_updates_speed_and_fix() {
    const bool accepted = gpsRuntimeModule.injectNmeaSentenceForTest(
        "$GPRMC,123519,A,4807.038,N,01131.000,E,010.0,084.4,230394,003.1,W*6F", 1000);
    TEST_ASSERT_TRUE(accepted);

    GpsRuntimeStatus status = gpsRuntimeModule.snapshot(1000);
    TEST_ASSERT_TRUE(status.sampleValid);
    TEST_ASSERT_TRUE(status.hasFix);
    TEST_ASSERT_TRUE(status.locationValid);
    TEST_ASSERT_EQUAL_UINT32(0, status.fixAgeMs);
    TEST_ASSERT_EQUAL_UINT32(1, status.hardwareSamples);
    TEST_ASSERT_FLOAT_WITHIN(0.02f, 11.50779f, status.speedMph);
    TEST_ASSERT_FLOAT_WITHIN(0.0002f, 48.1173f, status.latitudeDeg);
    TEST_ASSERT_FLOAT_WITHIN(0.0002f, 11.516667f, status.longitudeDeg);
    TEST_ASSERT_TRUE(status.courseValid);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 84.4f, status.courseDeg);
    TEST_ASSERT_EQUAL_UINT32(1000, status.courseSampleTsMs);
    TEST_ASSERT_EQUAL_UINT32(0, status.courseAgeMs);

    float speedMph = 0.0f;
    uint32_t tsMs = 0;
    TEST_ASSERT_TRUE(gpsRuntimeModule.getFreshSpeed(2500, speedMph, tsMs));
    TEST_ASSERT_FLOAT_WITHIN(0.02f, 11.50779f, speedMph);
    TEST_ASSERT_EQUAL_UINT32(1000, tsMs);
}

void test_valid_rmc_with_missing_course_marks_course_invalid() {
    const bool accepted = gpsRuntimeModule.injectNmeaSentenceForTest(
        "$GPRMC,123519,A,4807.038,N,01131.000,E,010.0,,230394,003.1,W*49", 2200);
    TEST_ASSERT_TRUE(accepted);

    GpsRuntimeStatus status = gpsRuntimeModule.snapshot(2200);
    TEST_ASSERT_TRUE(status.sampleValid);
    TEST_ASSERT_TRUE(status.hasFix);
    TEST_ASSERT_FALSE(status.courseValid);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, status.courseAgeMs);
}

void test_scaffold_sample_accepts_optional_course() {
    gpsRuntimeModule.setScaffoldSample(25.0f,
                                       true,
                                       7,
                                       0.8f,
                                       3000,
                                       37.25f,
                                       -122.01f,
                                       123.5f);
    const GpsRuntimeStatus status = gpsRuntimeModule.snapshot(3000);
    TEST_ASSERT_TRUE(status.courseValid);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 123.5f, status.courseDeg);
    TEST_ASSERT_EQUAL_UINT32(3000, status.courseSampleTsMs);
}

void test_valid_gga_sets_quality_without_speed_sample() {
    const bool accepted = gpsRuntimeModule.injectNmeaSentenceForTest(
        "$GPGGA,123520,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*4D", 2000);
    TEST_ASSERT_TRUE(accepted);

    GpsRuntimeStatus status = gpsRuntimeModule.snapshot(2000);
    TEST_ASSERT_TRUE(status.hasFix);
    TEST_ASSERT_FALSE(status.sampleValid);
    TEST_ASSERT_EQUAL_UINT8(8, status.satellites);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.9f, status.hdop);
    TEST_ASSERT_TRUE(status.locationValid);
    TEST_ASSERT_EQUAL_UINT32(0, status.fixAgeMs);
    TEST_ASSERT_FLOAT_WITHIN(0.0002f, 48.1173f, status.latitudeDeg);
    TEST_ASSERT_FLOAT_WITHIN(0.0002f, 11.516667f, status.longitudeDeg);
}

void test_bad_checksum_is_rejected_and_counted() {
    const bool accepted = gpsRuntimeModule.injectNmeaSentenceForTest(
        "$GPRMC,123519,A,4807.038,N,01131.000,E,010.0,084.4,230394,003.1,W*00", 3000);
    TEST_ASSERT_FALSE(accepted);

    GpsRuntimeStatus status = gpsRuntimeModule.snapshot(3000);
    TEST_ASSERT_EQUAL_UINT32(1, status.sentencesSeen);
    TEST_ASSERT_EQUAL_UINT32(1, status.checksumFailures);
    TEST_ASSERT_EQUAL_UINT32(1, status.parseFailures);
    TEST_ASSERT_FALSE(status.sampleValid);
}

void test_fix_loss_invalidates_speed_sample() {
    TEST_ASSERT_TRUE(gpsRuntimeModule.injectNmeaSentenceForTest(
        "$GPRMC,123519,A,4807.038,N,01131.000,E,010.0,084.4,230394,003.1,W*6F", 1000));
    TEST_ASSERT_TRUE(gpsRuntimeModule.injectNmeaSentenceForTest(
        "$GPRMC,123521,V,4807.038,N,01131.000,E,000.0,000.0,230394,003.1,W*7A", 2000));

    GpsRuntimeStatus status = gpsRuntimeModule.snapshot(2000);
    TEST_ASSERT_FALSE(status.sampleValid);
    TEST_ASSERT_FALSE(status.hasFix);
    TEST_ASSERT_FALSE(status.locationValid);
}

void test_invalid_coordinate_is_rejected() {
    const bool accepted = gpsRuntimeModule.injectNmeaSentenceForTest(
        "$GPGGA,123520,4807.038,N,01161.000,E,1,08,0.9,545.4,M,46.9,M,,*48", 2500);
    TEST_ASSERT_FALSE(accepted);

    GpsRuntimeStatus status = gpsRuntimeModule.snapshot(2500);
    TEST_ASSERT_FALSE(status.hasFix);
    TEST_ASSERT_FALSE(status.locationValid);
    TEST_ASSERT_EQUAL_UINT32(1, status.parseFailures);
}

void test_detection_timeout_disables_runtime_polling() {
    gpsRuntimeModule.update(61010);

    GpsRuntimeStatus status = gpsRuntimeModule.snapshot(61010);
    TEST_ASSERT_TRUE(status.detectionTimedOut);
    TEST_ASSERT_FALSE(status.parserActive);
    TEST_ASSERT_FALSE(status.moduleDetected);
}

void test_stale_fix_is_cleared() {
    TEST_ASSERT_TRUE(gpsRuntimeModule.injectNmeaSentenceForTest(
        "$GPRMC,123519,A,4807.038,N,01131.000,E,010.0,084.4,230394,003.1,W*6F", 1000));

    gpsRuntimeModule.update(17002);
    GpsRuntimeStatus status = gpsRuntimeModule.snapshot(17002);
    TEST_ASSERT_FALSE(status.hasFix);
    TEST_ASSERT_FALSE(status.sampleValid);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, status.fixAgeMs);
}

void test_stable_fix_holds_briefly_after_fix_drop() {
    gpsRuntimeModule.setScaffoldSample(32.0f, true, 8, 0.9f, 1000);
    mockMillis = 2000;
    gpsRuntimeModule.clearSample();

    GpsRuntimeStatus held = gpsRuntimeModule.snapshot(3500);
    TEST_ASSERT_FALSE(held.hasFix);
    TEST_ASSERT_TRUE(held.stableHasFix);
    TEST_ASSERT_EQUAL_UINT8(8, held.stableSatellites);

    GpsRuntimeStatus expired = gpsRuntimeModule.snapshot(5005);
    TEST_ASSERT_FALSE(expired.stableHasFix);
    TEST_ASSERT_EQUAL_UINT8(0, expired.stableSatellites);
}

void test_stable_satellites_slew_toward_raw_count() {
    gpsRuntimeModule.setScaffoldSample(25.0f, true, 6, 0.8f, 1000);
    GpsRuntimeStatus first = gpsRuntimeModule.snapshot(1000);
    TEST_ASSERT_EQUAL_UINT8(6, first.satellites);
    TEST_ASSERT_EQUAL_UINT8(6, first.stableSatellites);

    gpsRuntimeModule.setScaffoldSample(25.0f, true, 10, 0.8f, 2000);
    GpsRuntimeStatus second = gpsRuntimeModule.snapshot(2000);
    TEST_ASSERT_EQUAL_UINT8(10, second.satellites);
    TEST_ASSERT_EQUAL_UINT8(7, second.stableSatellites);

    gpsRuntimeModule.setScaffoldSample(25.0f, true, 10, 0.8f, 3000);
    GpsRuntimeStatus third = gpsRuntimeModule.snapshot(3000);
    TEST_ASSERT_EQUAL_UINT8(8, third.stableSatellites);
}

void test_parseRmcDateTime_valid_date() {
    // 2026-02-15 12:35:19.00 UTC
    int64_t epochMs = 0;
    TEST_ASSERT_TRUE(GpsRuntimeModule::parseRmcDateTime("123519.00", "150226", epochMs));
    // 2026-02-15 12:35:19 UTC = 1771158919 seconds since epoch
    TEST_ASSERT_TRUE(epochMs >= 1771158919000LL - 1000LL);
    TEST_ASSERT_TRUE(epochMs <= 1771158919000LL + 1000LL);
}

void test_parseRmcDateTime_known_epoch() {
    // 2024-01-01 00:00:00 UTC = 1704067200 seconds since epoch
    int64_t epochMs = 0;
    TEST_ASSERT_TRUE(GpsRuntimeModule::parseRmcDateTime("000000", "010124", epochMs));
    TEST_ASSERT_TRUE(epochMs == 1704067200000LL);
}

void test_parseRmcDateTime_fractional_seconds() {
    int64_t epochMs = 0;
    TEST_ASSERT_TRUE(GpsRuntimeModule::parseRmcDateTime("000000.50", "010124", epochMs));
    TEST_ASSERT_TRUE(epochMs == 1704067200500LL);
}

void test_parseRmcDateTime_rejects_bad_date() {
    int64_t epochMs = 0;
    // Month 13 is invalid
    TEST_ASSERT_FALSE(GpsRuntimeModule::parseRmcDateTime("120000", "011324", epochMs));
    // April 31 is invalid
    TEST_ASSERT_FALSE(GpsRuntimeModule::parseRmcDateTime("120000", "310424", epochMs));
    // February 29 is invalid on non-leap years
    TEST_ASSERT_FALSE(GpsRuntimeModule::parseRmcDateTime("120000", "290225", epochMs));
    // February 29 is valid on leap years
    TEST_ASSERT_TRUE(GpsRuntimeModule::parseRmcDateTime("120000", "290224", epochMs));
    // Empty fields
    TEST_ASSERT_FALSE(GpsRuntimeModule::parseRmcDateTime("", "010124", epochMs));
    TEST_ASSERT_FALSE(GpsRuntimeModule::parseRmcDateTime("120000", "", epochMs));
    // Short date
    TEST_ASSERT_FALSE(GpsRuntimeModule::parseRmcDateTime("120000", "0101", epochMs));
}

void test_gps_time_injection_rate_limited() {
    // First valid RMC should succeed and mark time update.
    const bool first = gpsRuntimeModule.injectNmeaSentenceForTest(
        "$GPRMC,123519,A,4807.038,N,01131.000,E,010.0,084.4,150226,003.1,W*62", 1000);
    TEST_ASSERT_TRUE(first);

    // Second RMC 10s later — still within 60s rate limit.
    const bool second = gpsRuntimeModule.injectNmeaSentenceForTest(
        "$GPRMC,123529,A,4807.038,N,01131.000,E,010.0,084.4,150226,003.1,W*61", 11000);
    TEST_ASSERT_TRUE(second);

    // Third RMC 61s after the first — should trigger time update again.
    const bool third = gpsRuntimeModule.injectNmeaSentenceForTest(
        "$GPRMC,124119,A,4807.038,N,01131.000,E,010.0,084.4,150226,003.1,W*61", 62000);
    TEST_ASSERT_TRUE(third);
}

void test_overlong_sentence_is_rejected() {
    char longSentence[160];
    for (size_t i = 0; i < sizeof(longSentence) - 1; ++i) {
        longSentence[i] = 'A';
    }
    longSentence[sizeof(longSentence) - 1] = '\0';

    TEST_ASSERT_FALSE(gpsRuntimeModule.injectNmeaSentenceForTest(longSentence, 4000));
    GpsRuntimeStatus status = gpsRuntimeModule.snapshot(4000);
    TEST_ASSERT_EQUAL_UINT32(1, status.bufferOverruns);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_valid_rmc_updates_speed_and_fix);
    RUN_TEST(test_valid_gga_sets_quality_without_speed_sample);
    RUN_TEST(test_valid_rmc_with_missing_course_marks_course_invalid);
    RUN_TEST(test_scaffold_sample_accepts_optional_course);
    RUN_TEST(test_bad_checksum_is_rejected_and_counted);
    RUN_TEST(test_fix_loss_invalidates_speed_sample);
    RUN_TEST(test_invalid_coordinate_is_rejected);
    RUN_TEST(test_detection_timeout_disables_runtime_polling);
    RUN_TEST(test_stale_fix_is_cleared);
    RUN_TEST(test_stable_fix_holds_briefly_after_fix_drop);
    RUN_TEST(test_stable_satellites_slew_toward_raw_count);
    RUN_TEST(test_overlong_sentence_is_rejected);
    RUN_TEST(test_parseRmcDateTime_valid_date);
    RUN_TEST(test_parseRmcDateTime_known_epoch);
    RUN_TEST(test_parseRmcDateTime_fractional_seconds);
    RUN_TEST(test_parseRmcDateTime_rejects_bad_date);
    RUN_TEST(test_gps_time_injection_rate_limited);
    return UNITY_END();
}
