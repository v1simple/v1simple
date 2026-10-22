#include <unity.h>

#include "../mocks/Arduino.h"
#include "../../src/touch_handler.cpp"

SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;

namespace {
TouchHandler touch;
int16_t readX = -1;
int16_t readY = -1;

bool poll(unsigned long now, uint16_t x, uint16_t y, uint8_t points = 1) {
    std::vector<uint8_t> data(32, 0);
    data[1] = points;
    data[2] = x >> 8;
    data[3] = x;
    data[4] = y >> 8;
    data[5] = y;
    Wire.queueRequestFrom(data.size(), data);
    mockMillis = now;
    return touch.getTouchPoint(readX, readY);
}

bool pollRawFrame(unsigned long now, const std::vector<uint8_t>& frame) {
    Wire.queueRequestFrom(frame.size(), frame);
    mockMillis = now;
    return touch.getTouchPoint(readX, readY);
}

void assert_invalid_contact_count_preserves_contact(uint8_t count) {
    TEST_ASSERT_TRUE(poll(1000, 100, 100));
    TEST_ASSERT_TRUE(touch.isTouchActive());
    TEST_ASSERT_FALSE(poll(1250, 100, 100, count));
    TEST_ASSERT_FALSE(touch.isTouchActive());
    TEST_ASSERT_FALSE(poll(1500, 100, 100));
    TEST_ASSERT_TRUE(touch.isTouchActive());
    TEST_ASSERT_FALSE(poll(1550, 0, 0, 0));
    TEST_ASSERT_FALSE(touch.isTouchActive());
    TEST_ASSERT_TRUE(poll(1650, 100, 100));
    TEST_ASSERT_TRUE(touch.isTouchActive());
    TEST_ASSERT_EQUAL_INT(0, Wire.endCalls);
}
} // namespace

void setUp() {
    mockMillis = 0;
    Wire.resetMock();
    touch = TouchHandler{};
    TEST_ASSERT_TRUE(touch.begin());
}
void tearDown() {}

void test_impossible_coordinates_are_invalid_without_bus_recovery() {
    const uint16_t points[][2] = {{514, 514}, {641, 100}, {100, 173}, {4095, 4095}};
    for (const auto& point : points) {
        setUp();
        for (unsigned long now = 1000; now <= 2000; now += 250) {
            TEST_ASSERT_FALSE(poll(now, point[0], point[1]));
            TEST_ASSERT_FALSE(touch.isTouchActive());
        }
        TEST_ASSERT_EQUAL_INT(0, Wire.endCalls);
        TEST_ASSERT_TRUE(poll(2250, 100, 100));
    }
}

void test_supplier_raw_extrema_and_contact_counts_remain_accepted() {
    const uint16_t corners[][2] = {{0, 0}, {640, 0}, {0, 172}, {640, 172}};
    for (uint8_t count = 1; count <= 4; ++count) {
        for (const auto& point : corners) {
            setUp();
            TEST_ASSERT_TRUE(poll(1000, point[0], point[1], count));
            TEST_ASSERT_TRUE(touch.isTouchActive());
            TEST_ASSERT_EQUAL_INT16(point[0], readX);
            TEST_ASSERT_EQUAL_INT16(point[1], readY);
            TEST_ASSERT_FALSE(poll(1250, point[0], point[1], count));
        }
    }
}

void test_invalid_coordinates_during_contact_do_not_create_a_recovery_tap() {
    TEST_ASSERT_TRUE(poll(1000, 100, 100));
    TEST_ASSERT_FALSE(poll(1250, 514, 514));
    TEST_ASSERT_FALSE(touch.isTouchActive());
    TEST_ASSERT_FALSE(poll(1500, 150, 100));
    TEST_ASSERT_TRUE(touch.isTouchActive());
    TEST_ASSERT_FALSE(poll(1550, 0, 0, 0));
    TEST_ASSERT_FALSE(touch.isTouchActive());
    TEST_ASSERT_TRUE(poll(1750, 150, 100));
}

void test_five_contacts_during_hold_do_not_create_a_recovery_tap() {
    assert_invalid_contact_count_preserves_contact(5);
}

void test_max_contact_count_during_hold_does_not_create_a_recovery_tap() {
    assert_invalid_contact_count_preserves_contact(255);
}

void test_required_release_ignores_contacts_and_invalid_observations() {
    for (bool previouslyDown : {false, true}) {
        setUp();
        if (previouslyDown) {
            TEST_ASSERT_TRUE(poll(1000, 100, 100));
        }
        touch.requireRelease();
        TEST_ASSERT_FALSE(touch.isTouchActive());
        TEST_ASSERT_FALSE(poll(1250, 100, 100));
        TEST_ASSERT_FALSE(touch.isTouchActive());
        TEST_ASSERT_FALSE(poll(1500, 514, 514));
        TEST_ASSERT_FALSE(poll(1750, 0, 0, 5));

        mockMillis = 2000;
        Wire.queueRequestFrom(0, {});
        TEST_ASSERT_FALSE(touch.getTouchPoint(readX, readY));
        mockMillis = 2250;
        Wire.queueEndTransmission(2);
        TEST_ASSERT_FALSE(touch.getTouchPoint(readX, readY));
        TEST_ASSERT_FALSE(poll(2500, 100, 100));
        TEST_ASSERT_FALSE(touch.isTouchActive());

        TEST_ASSERT_FALSE(poll(2750, 0, 0, 0)); // Only an observed release unlocks.
        TEST_ASSERT_FALSE(poll(2800, 100, 100)); // Preserve the 100 ms release debounce.
        TEST_ASSERT_FALSE(poll(3000, 100, 100)); // Early recontact stays suppressed.
        TEST_ASSERT_FALSE(poll(3050, 0, 0, 0));
        TEST_ASSERT_TRUE(poll(3150, 100, 100));
    }
}

static uint32_t beginWithNack(uint32_t now = 10000) {
    mockMillis = now;
    Wire.resetMock();
    touch = TouchHandler{};
    Wire.queueEndTransmission(2);
    TEST_ASSERT_FALSE(touch.begin());
    TEST_ASSERT_FALSE(touch.isAvailable());
    TEST_ASSERT_FALSE(touch.isTouchActive());
    return static_cast<uint32_t>(mockMillis);
}

void test_failed_startup_retries_slowly_then_requires_release_before_contact() {
    const uint32_t started = beginWithNack();
    const int initialTransactions = Wire.endTransmissionCalls;
    for (uint32_t elapsed = 1; elapsed < 1000; ++elapsed) {
        mockMillis = started + elapsed;
        TEST_ASSERT_FALSE(touch.getTouchPoint(readX, readY));
    }
    TEST_ASSERT_EQUAL_INT(initialTransactions, Wire.endTransmissionCalls);
    for (uint32_t retry = 1; retry <= 3; ++retry) {
        mockMillis = started + retry * 1000;
        Wire.queueEndTransmission(2);
        TEST_ASSERT_FALSE(touch.getTouchPoint(readX, readY));
        TEST_ASSERT_FALSE(touch.isAvailable());
        const int attempts = Wire.endTransmissionCalls;
        for (uint32_t elapsed = 1; elapsed < 1000; elapsed += 10) {
            mockMillis = started + retry * 1000 + elapsed;
            TEST_ASSERT_FALSE(touch.getTouchPoint(readX, readY));
        }
        TEST_ASSERT_EQUAL_INT(attempts, Wire.endTransmissionCalls);
    }
    TEST_ASSERT_GREATER_THAN_INT(0, Wire.endCalls); // Persistent faults still get bus recovery.
    TEST_ASSERT_FALSE(poll(started + 4000, 120, 80)); // Held during recovery: no synthetic tap.
    TEST_ASSERT_TRUE(touch.isAvailable());
    TEST_ASSERT_FALSE(touch.isTouchActive());
    TEST_ASSERT_FALSE(poll(started + 4100, 120, 80));
    TEST_ASSERT_FALSE(poll(started + 4200, 0, 0, 0));
    TEST_ASSERT_TRUE(poll(started + 4300, 120, 80));
    TEST_ASSERT_TRUE(touch.isTouchActive());
    TEST_ASSERT_EQUAL_INT16(120, readX);
    TEST_ASSERT_EQUAL_INT16(80, readY);
}

void test_startup_discovery_rejects_short_or_invalid_frames_until_valid_release() {
    const uint32_t started = beginWithNack();
    mockMillis = started + 1000;
    Wire.queueRequestFrom(1, {0});
    TEST_ASSERT_FALSE(touch.getTouchPoint(readX, readY));
    TEST_ASSERT_FALSE(touch.isAvailable());
    TEST_ASSERT_FALSE(poll(started + 2000, 100, 100, 255));
    TEST_ASSERT_FALSE(touch.isAvailable());
    TEST_ASSERT_FALSE(poll(started + 3000, 641, 100));
    TEST_ASSERT_FALSE(touch.isAvailable());
    TEST_ASSERT_FALSE(poll(started + 4000, 0, 0, 0));
    TEST_ASSERT_TRUE(touch.isAvailable());
    TEST_ASSERT_TRUE(poll(started + 4100, 100, 100));
}

void test_recovered_touch_keeps_runtime_fault_recovery_without_duplicate_taps() {
    const uint32_t started = beginWithNack();
    TEST_ASSERT_FALSE(poll(started + 1000, 0, 0, 0));
    TEST_ASSERT_TRUE(poll(started + 1100, 100, 100));
    for (uint32_t elapsed : {1200, 1210, 1220}) {
        mockMillis = started + elapsed;
        Wire.queueEndTransmission(2);
        TEST_ASSERT_FALSE(touch.getTouchPoint(readX, readY));
        TEST_ASSERT_FALSE(touch.isTouchActive());
    }
    TEST_ASSERT_GREATER_THAN_INT(0, Wire.endCalls);
    TEST_ASSERT_FALSE(poll(started + 1300, 100, 100));
    TEST_ASSERT_TRUE(touch.isTouchActive());
    TEST_ASSERT_FALSE(poll(started + 1400, 0, 0, 0));
    TEST_ASSERT_TRUE(poll(started + 1500, 100, 100));
}

void test_discovery_retry_and_release_debounce_cross_millis_rollover() {
    const uint32_t started = beginWithNack(UINT32_MAX - 500);
    const int attempts = Wire.endTransmissionCalls;
    mockMillis = static_cast<uint32_t>(started + 999);
    TEST_ASSERT_FALSE(touch.getTouchPoint(readX, readY));
    TEST_ASSERT_EQUAL_INT(attempts, Wire.endTransmissionCalls);
    TEST_ASSERT_FALSE(poll(static_cast<uint32_t>(started + 1000), 0, 0, 0));
    TEST_ASSERT_TRUE(touch.isAvailable());
    TEST_ASSERT_TRUE(poll(static_cast<uint32_t>(started + 1100), 100, 100));
}

void test_uninitialized_touch_never_probes_bus() {
    Wire.resetMock();
    touch = TouchHandler{};
    mockMillis = 100000;
    TEST_ASSERT_FALSE(touch.getTouchPoint(readX, readY));
    TEST_ASSERT_EQUAL_INT(0, Wire.endTransmissionCalls);
}

void test_full_idle_fill_report_releases_contact_and_allows_the_next_tap() {
    const std::vector<uint8_t> idle(32, 0x38);
    TEST_ASSERT_TRUE(poll(1000, 100, 100));
    TEST_ASSERT_FALSE(pollRawFrame(1050, idle));
    TEST_ASSERT_FALSE(touch.isTouchActive());
    TEST_ASSERT_FALSE(pollRawFrame(1100, idle));
    TEST_ASSERT_TRUE(poll(1250, 120, 80));
    TEST_ASSERT_TRUE(touch.isTouchActive());
    TEST_ASSERT_EQUAL_INT16(120, readX);
    TEST_ASSERT_EQUAL_INT16(80, readY);
    TEST_ASSERT_EQUAL_INT(0, Wire.endCalls);
}
void test_near_miss_idle_fill_and_count_only_reports_do_not_release_a_held_contact() {
    for (size_t changedByte = 0; changedByte < 32; ++changedByte) {
        setUp();
        TEST_ASSERT_TRUE(poll(1000, 100, 100));
        std::vector<uint8_t> nearMiss(32, 0x38);
        nearMiss[changedByte] = 0x39;
        TEST_ASSERT_FALSE(pollRawFrame(1250, nearMiss));
        TEST_ASSERT_FALSE(touch.isTouchActive());
        TEST_ASSERT_FALSE(poll(1500, 100, 100));
        TEST_ASSERT_TRUE(touch.isTouchActive());
        TEST_ASSERT_EQUAL_INT(0, Wire.endCalls);
    }
    setUp();
    assert_invalid_contact_count_preserves_contact(0x38);
}
void test_truncated_idle_fill_is_a_read_failure_and_does_not_release_contact() {
    TEST_ASSERT_TRUE(poll(1000, 100, 100));
    TEST_ASSERT_FALSE(pollRawFrame(1250, std::vector<uint8_t>(31, 0x38)));
    TEST_ASSERT_FALSE(touch.isTouchActive());
    TEST_ASSERT_FALSE(poll(1500, 100, 100));
    TEST_ASSERT_TRUE(touch.isTouchActive());
    TEST_ASSERT_EQUAL_INT(0, Wire.endCalls);
}
void test_required_release_accepts_full_idle_fill_with_existing_release_debounce() {
    for (bool previouslyDown : {false, true}) {
        setUp();
        if (previouslyDown) TEST_ASSERT_TRUE(poll(1000, 100, 100));
        touch.requireRelease();
        TEST_ASSERT_FALSE(poll(1250, 100, 100));
        TEST_ASSERT_FALSE(touch.isTouchActive());
        TEST_ASSERT_FALSE(pollRawFrame(1500, std::vector<uint8_t>(32, 0x38)));
        TEST_ASSERT_FALSE(touch.isTouchActive());
        TEST_ASSERT_FALSE(poll(1550, 100, 100)); // Too soon after release.
        TEST_ASSERT_FALSE(poll(1750, 100, 100)); // Early recontact is one held finger.
        TEST_ASSERT_FALSE(pollRawFrame(1800, std::vector<uint8_t>(32, 0x38)));
        TEST_ASSERT_TRUE(poll(1900, 100, 100));
        TEST_ASSERT_TRUE(touch.isTouchActive());
    }
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_failed_startup_retries_slowly_then_requires_release_before_contact);
    RUN_TEST(test_startup_discovery_rejects_short_or_invalid_frames_until_valid_release);
    RUN_TEST(test_recovered_touch_keeps_runtime_fault_recovery_without_duplicate_taps);
    RUN_TEST(test_discovery_retry_and_release_debounce_cross_millis_rollover);
    RUN_TEST(test_uninitialized_touch_never_probes_bus);
    RUN_TEST(test_impossible_coordinates_are_invalid_without_bus_recovery);
    RUN_TEST(test_supplier_raw_extrema_and_contact_counts_remain_accepted);
    RUN_TEST(test_invalid_coordinates_during_contact_do_not_create_a_recovery_tap);
    RUN_TEST(test_five_contacts_during_hold_do_not_create_a_recovery_tap);
    RUN_TEST(test_max_contact_count_during_hold_does_not_create_a_recovery_tap);
    RUN_TEST(test_required_release_ignores_contacts_and_invalid_observations);
    RUN_TEST(test_full_idle_fill_report_releases_contact_and_allows_the_next_tap);
    RUN_TEST(test_near_miss_idle_fill_and_count_only_reports_do_not_release_a_held_contact);
    RUN_TEST(test_truncated_idle_fill_is_a_read_failure_and_does_not_release_contact);
    RUN_TEST(test_required_release_accepts_full_idle_fill_with_existing_release_debounce);
    return UNITY_END();
}
