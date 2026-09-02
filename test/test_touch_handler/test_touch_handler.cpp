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

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_impossible_coordinates_are_invalid_without_bus_recovery);
    RUN_TEST(test_supplier_raw_extrema_and_contact_counts_remain_accepted);
    RUN_TEST(test_invalid_coordinates_during_contact_do_not_create_a_recovery_tap);
    RUN_TEST(test_required_release_ignores_contacts_and_invalid_observations);
    return UNITY_END();
}
