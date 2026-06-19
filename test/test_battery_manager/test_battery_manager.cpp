#include <unity.h>

#include "../../include/battery_math.h"

namespace {

using battery_math::isCritical;
using battery_math::isLow;
using battery_math::kCriticalMv;
using battery_math::kEmptyMv;
using battery_math::kFullMv;
using battery_math::kWarningMv;
using battery_math::voltageToPercent;

}  // namespace

void setUp() {}
void tearDown() {}

void test_percent_caps_at_full_and_above() {
    TEST_ASSERT_EQUAL_UINT8(100, voltageToPercent(kFullMv));
    TEST_ASSERT_EQUAL_UINT8(100, voltageToPercent(4200));
    TEST_ASSERT_EQUAL_UINT8(100, voltageToPercent(5000));
}

void test_percent_floors_at_empty_and_below() {
    TEST_ASSERT_EQUAL_UINT8(0, voltageToPercent(kEmptyMv));
    TEST_ASSERT_EQUAL_UINT8(0, voltageToPercent(3000));
    TEST_ASSERT_EQUAL_UINT8(0, voltageToPercent(0));
}

void test_percent_is_linear_between_empty_and_full() {
    const uint16_t midpoint = static_cast<uint16_t>(kEmptyMv + (kFullMv - kEmptyMv) / 2);
    const uint16_t quarter = static_cast<uint16_t>(kEmptyMv + (kFullMv - kEmptyMv) / 4);
    const uint16_t threeQuarter = static_cast<uint16_t>(kEmptyMv + ((kFullMv - kEmptyMv) * 3) / 4);

    TEST_ASSERT_UINT8_WITHIN(1, 50, voltageToPercent(midpoint));
    TEST_ASSERT_UINT8_WITHIN(1, 25, voltageToPercent(quarter));
    TEST_ASSERT_UINT8_WITHIN(1, 75, voltageToPercent(threeQuarter));
}

void test_percent_uses_expected_threshold_examples() {
    TEST_ASSERT_UINT8_WITHIN(1, 22, voltageToPercent(kWarningMv));
    TEST_ASSERT_UINT8_WITHIN(1, 5, voltageToPercent(kCriticalMv));
    TEST_ASSERT_UINT8_WITHIN(1, 99, voltageToPercent(static_cast<uint16_t>(kFullMv - 1)));
    TEST_ASSERT_EQUAL_UINT8(0, voltageToPercent(static_cast<uint16_t>(kEmptyMv + 1)));
}

void test_percent_is_monotonic_across_range() {
    uint8_t last = voltageToPercent(kEmptyMv);
    for (uint16_t voltage = static_cast<uint16_t>(kEmptyMv + 25); voltage <= kFullMv; voltage += 25) {
        const uint8_t next = voltageToPercent(voltage);
        TEST_ASSERT_GREATER_OR_EQUAL_UINT8(last, next);
        last = next;
    }
}

void test_low_uses_strict_warning_boundary() {
    TEST_ASSERT_TRUE(isLow(static_cast<uint16_t>(kWarningMv - 1)));
    TEST_ASSERT_FALSE(isLow(kWarningMv));
    TEST_ASSERT_FALSE(isLow(static_cast<uint16_t>(kWarningMv + 1)));
    TEST_ASSERT_FALSE(isLow(0));
}

void test_critical_uses_strict_critical_boundary() {
    TEST_ASSERT_TRUE(isCritical(static_cast<uint16_t>(kCriticalMv - 1)));
    TEST_ASSERT_FALSE(isCritical(kCriticalMv));
    TEST_ASSERT_FALSE(isCritical(static_cast<uint16_t>(kCriticalMv + 1)));
    TEST_ASSERT_FALSE(isCritical(0));
}

void test_critical_implies_low_for_nonzero_voltage() {
    for (uint16_t voltage = 1; voltage < kCriticalMv; voltage += 100) {
        TEST_ASSERT_TRUE(isCritical(voltage));
        TEST_ASSERT_TRUE(isLow(voltage));
    }
}

void test_threshold_values_match_expected_device_profile() {
    TEST_ASSERT_EQUAL_UINT16(4095, kFullMv);
    TEST_ASSERT_EQUAL_UINT16(3200, kEmptyMv);
    TEST_ASSERT_EQUAL_UINT16(3400, kWarningMv);
    TEST_ASSERT_EQUAL_UINT16(3250, kCriticalMv);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_percent_caps_at_full_and_above);
    RUN_TEST(test_percent_floors_at_empty_and_below);
    RUN_TEST(test_percent_is_linear_between_empty_and_full);
    RUN_TEST(test_percent_uses_expected_threshold_examples);
    RUN_TEST(test_percent_is_monotonic_across_range);
    RUN_TEST(test_low_uses_strict_warning_boundary);
    RUN_TEST(test_critical_uses_strict_critical_boundary);
    RUN_TEST(test_critical_implies_low_for_nonzero_voltage);
    RUN_TEST(test_threshold_values_match_expected_device_profile);
    return UNITY_END();
}
