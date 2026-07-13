#include <unity.h>

#include "../../src/modules/gps/gps_observation_log.h"
#include "../../src/modules/gps/gps_observation_log.cpp"  // Pull implementation for UNIT_TEST.

#ifndef ARDUINO
SerialClass Serial;
#endif

void setUp() {
    gpsObservationLog.reset();
}

void tearDown() {}

void test_publish_and_copy_recent_order() {
    GpsObservation a;
    a.tsMs = 100;
    a.hasFix = true;
    a.speedValid = true;
    a.speedMph = 10.0f;

    GpsObservation b;
    b.tsMs = 200;
    b.hasFix = true;
    b.speedValid = true;
    b.speedMph = 20.0f;

    GpsObservation c;
    c.tsMs = 300;
    c.hasFix = true;
    c.speedValid = true;
    c.speedMph = 30.0f;

    TEST_ASSERT_TRUE(gpsObservationLog.publish(a));
    TEST_ASSERT_TRUE(gpsObservationLog.publish(b));
    TEST_ASSERT_TRUE(gpsObservationLog.publish(c));

    GpsObservation out[3] = {};
    const size_t count = gpsObservationLog.copyRecent(out, 3);
    TEST_ASSERT_EQUAL_UINT32(3, static_cast<uint32_t>(count));
    TEST_ASSERT_EQUAL_UINT32(300, out[0].tsMs);
    TEST_ASSERT_EQUAL_UINT32(200, out[1].tsMs);
    TEST_ASSERT_EQUAL_UINT32(100, out[2].tsMs);
}

void test_overflow_drops_oldest() {
    for (uint32_t i = 1; i <= (GpsObservationLog::kCapacity + 1); ++i) {
        GpsObservation sample;
        sample.tsMs = i;
        gpsObservationLog.publish(sample);
    }

    const GpsObservationLogStats stats = gpsObservationLog.stats();
    TEST_ASSERT_EQUAL_UINT32(GpsObservationLog::kCapacity + 1, stats.published);
    TEST_ASSERT_EQUAL_UINT32(1, stats.drops);
    TEST_ASSERT_EQUAL_UINT32(GpsObservationLog::kCapacity, static_cast<uint32_t>(stats.size));

    GpsObservation out[GpsObservationLog::kCapacity] = {};
    const size_t count = gpsObservationLog.copyRecent(out, GpsObservationLog::kCapacity);
    TEST_ASSERT_EQUAL_UINT32(GpsObservationLog::kCapacity, static_cast<uint32_t>(count));
    TEST_ASSERT_EQUAL_UINT32(GpsObservationLog::kCapacity + 1, out[0].tsMs);
    TEST_ASSERT_EQUAL_UINT32(2, out[count - 1].tsMs);
}

void test_copy_recent_respects_limit() {
    for (uint32_t i = 1; i <= 5; ++i) {
        GpsObservation sample;
        sample.tsMs = i * 10;
        gpsObservationLog.publish(sample);
    }

    GpsObservation out[2] = {};
    const size_t count = gpsObservationLog.copyRecent(out, 2);
    TEST_ASSERT_EQUAL_UINT32(2, static_cast<uint32_t>(count));
    TEST_ASSERT_EQUAL_UINT32(50, out[0].tsMs);
    TEST_ASSERT_EQUAL_UINT32(40, out[1].tsMs);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_publish_and_copy_recent_order);
    RUN_TEST(test_overflow_drops_oldest);
    RUN_TEST(test_copy_recent_respects_limit);
    return UNITY_END();
}
