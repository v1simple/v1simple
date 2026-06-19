#include <unity.h>

#include "../../src/modules/gps/gps_publishers.cpp"

void setUp() {}
void tearDown() {}

// ─────────────────────────────────────────────────────────────────────────────
// GpsTimePublisher tests
// ─────────────────────────────────────────────────────────────────────────────

void test_time_publisher_fresh_read_returns_valid_snapshot() {
    GpsTimePublisher pub;
    GpsTimeSnapshot snap;
    snap.valid       = true;
    snap.capturedMs  = 1000;
    snap.utcEpochMs  = 1700000000000ULL;  // ~Nov 2023
    snap.source      = 1;
    pub.publish(snap);

    uint64_t utcOut = 0;
    const bool ok = pub.readUtc(1000, utcOut);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_UINT64(1700000000000ULL, utcOut);
}

void test_time_publisher_extrapolates_utc_with_elapsed() {
    GpsTimePublisher pub;
    GpsTimeSnapshot snap;
    snap.valid      = true;
    snap.capturedMs = 1000;
    snap.utcEpochMs = 1700000000000ULL;
    snap.source     = 1;
    pub.publish(snap);

    // Read 2000 ms later — UTC should advance by 2000 ms
    uint64_t utcOut = 0;
    const bool ok = pub.readUtc(3000, utcOut);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_UINT64(1700000002000ULL, utcOut);
}

void test_time_publisher_stale_after_cutoff_returns_false() {
    GpsTimePublisher pub;
    GpsTimeSnapshot snap;
    snap.valid      = true;
    snap.capturedMs = 1000;
    snap.utcEpochMs = 1700000000000ULL;
    snap.source     = 1;
    pub.publish(snap);

    // Read well past kStaleMs (30000 ms)
    uint64_t utcOut = 0;
    const bool ok = pub.readUtc(1000 + GpsTimePublisher::kStaleMs + 1, utcOut);
    TEST_ASSERT_FALSE(ok);
}

void test_time_publisher_never_published_returns_false() {
    GpsTimePublisher pub;
    uint64_t utcOut = 0;
    const bool ok = pub.readUtc(5000, utcOut);
    TEST_ASSERT_FALSE(ok);
}

void test_time_publisher_invalid_snapshot_returns_false() {
    GpsTimePublisher pub;
    GpsTimeSnapshot snap;
    snap.valid      = false;
    snap.capturedMs = 1000;
    snap.utcEpochMs = 1700000000000ULL;
    snap.source     = 1;
    pub.publish(snap);

    uint64_t utcOut = 0;
    const bool ok = pub.readUtc(1500, utcOut);
    TEST_ASSERT_FALSE(ok);
}

void test_time_publisher_read_returns_raw_snapshot_without_extrapolation() {
    GpsTimePublisher pub;
    GpsTimeSnapshot snap;
    snap.valid      = true;
    snap.capturedMs = 2000;
    snap.utcEpochMs = 1700000010000ULL;
    snap.source     = 1;
    pub.publish(snap);

    // read() returns the raw snapshot; extrapolation happens in readUtc()
    const GpsTimeSnapshot result = pub.read(2500);
    TEST_ASSERT_TRUE(result.valid);
    TEST_ASSERT_EQUAL_UINT64(1700000010000ULL, result.utcEpochMs);
    TEST_ASSERT_EQUAL_UINT32(2000, result.capturedMs);

    // readUtc() at same nowMs=2500 should extrapolate +500 ms
    uint64_t extrapolated = 0;
    TEST_ASSERT_TRUE(pub.readUtc(2500, extrapolated));
    TEST_ASSERT_EQUAL_UINT64(1700000010500ULL, extrapolated);
}

// ─────────────────────────────────────────────────────────────────────────────
// GpsGeoPublisher tests
// ─────────────────────────────────────────────────────────────────────────────

void test_geo_publisher_fresh_snapshot_is_fresh() {
    GpsGeoPublisher pub;
    GpsGeoSnapshot geo;
    geo.valid      = true;
    geo.capturedMs = 1000;
    geo.hasFix     = true;
    pub.publish(geo);

    TEST_ASSERT_TRUE(pub.fresh(1000 + GpsGeoPublisher::kStaleMs - 1));
}

void test_geo_publisher_stale_snapshot_is_not_fresh() {
    GpsGeoPublisher pub;
    GpsGeoSnapshot geo;
    geo.valid      = true;
    geo.capturedMs = 1000;
    geo.hasFix     = true;
    pub.publish(geo);

    TEST_ASSERT_FALSE(pub.fresh(1000 + GpsGeoPublisher::kStaleMs + 1));
}

void test_geo_publisher_never_published_is_not_fresh() {
    GpsGeoPublisher pub;
    TEST_ASSERT_FALSE(pub.fresh(9999));
}

void test_geo_publisher_read_returns_published_snapshot() {
    GpsGeoPublisher pub;
    GpsGeoSnapshot geo;
    geo.valid        = true;
    geo.capturedMs   = 1000;
    geo.hasFix       = true;
    geo.latitudeDeg  = 48.1173f;
    geo.longitudeDeg = 11.5167f;
    geo.speedMph     = 62.5f;
    geo.satellites   = 8;
    pub.publish(geo);

    const GpsGeoSnapshot result = pub.read(1500);
    TEST_ASSERT_TRUE(result.valid);
    TEST_ASSERT_TRUE(result.hasFix);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 48.1173f, result.latitudeDeg);
    TEST_ASSERT_EQUAL_UINT8(8, result.satellites);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_time_publisher_fresh_read_returns_valid_snapshot);
    RUN_TEST(test_time_publisher_extrapolates_utc_with_elapsed);
    RUN_TEST(test_time_publisher_stale_after_cutoff_returns_false);
    RUN_TEST(test_time_publisher_never_published_returns_false);
    RUN_TEST(test_time_publisher_invalid_snapshot_returns_false);
    RUN_TEST(test_time_publisher_read_returns_raw_snapshot_without_extrapolation);
    RUN_TEST(test_geo_publisher_fresh_snapshot_is_fresh);
    RUN_TEST(test_geo_publisher_stale_snapshot_is_not_fresh);
    RUN_TEST(test_geo_publisher_never_published_is_not_fresh);
    RUN_TEST(test_geo_publisher_read_returns_published_snapshot);
    return UNITY_END();
}
