// Supply uncertainty must not change physical PWR input availability.
#include <unity.h>
#include <initializer_list>
#include "../../include/battery_source_policy.h"
#include "../../include/battery_math.h"
#include "../../include/poweroff_policy.h"
using namespace battery_source_policy;
void setUp() {}
void tearDown() {}

void test_unknown_source_keeps_cell_protection_without_claiming_a_supply() {
    TEST_ASSERT_EQUAL_STRING("unknown", sourceName(Source::Unknown));
    TEST_ASSERT_TRUE(resolveOnBattery(Source::Unknown));
    TEST_ASSERT_TRUE(battery_math::criticalProtectionRequired(resolveOnBattery(Source::Unknown), 3100));
    TEST_ASSERT_FALSE(battery_math::criticalProtectionRequired(resolveOnBattery(Source::Unknown), 0));
    TEST_ASSERT_FALSE(battery_math::criticalProtectionRequired(resolveOnBattery(Source::Unknown), 3800));
    TEST_ASSERT_EQUAL(poweroff_policy::Strategy::HARD_LATCH_CUT,
                      poweroff_policy::selectStrategy(resolveOnBattery(Source::Unknown)));
}

void test_button_availability_is_independent_of_supply_and_voltage() {
    for (Source source : {Source::Unknown, Source::Battery, Source::Usb}) {
        for (bool adc : {false, true}) {
            for (bool voltage : {false, true}) {
                TEST_ASSERT_TRUE(powerButtonHandlingEnabled({true, source, adc, voltage}));
                TEST_ASSERT_FALSE(powerButtonHandlingEnabled({false, source, adc, voltage}));
            }
        }
    }
}

void test_known_supply_names_and_policy_remain_distinct() {
    TEST_ASSERT_EQUAL_STRING("battery", sourceName(Source::Battery));
    TEST_ASSERT_EQUAL_STRING("usb", sourceName(Source::Usb));
    TEST_ASSERT_TRUE(resolveOnBattery(Source::Battery));
    TEST_ASSERT_FALSE(resolveOnBattery(Source::Usb));
}

void test_unknown_supply_fallback_selects_only_a_released_wake_input() {
    using namespace poweroff_policy;
    TEST_ASSERT_EQUAL(WakeInput::PWR_GPIO16, planBatteryFallbackWake(true).input);
    TEST_ASSERT_EQUAL(WakeInput::BOOT_GPIO0, planBatteryFallbackWake(false).input);
    for (bool pwr : {false, true}) {
        for (bool boot : {false, true}) {
            const auto plan = planBatteryFallbackWake(pwr);
            TEST_ASSERT_EQUAL(pwr || boot, wakePlanIsInactive(plan, pwr, boot));
        }
    }
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_unknown_source_keeps_cell_protection_without_claiming_a_supply);
    RUN_TEST(test_button_availability_is_independent_of_supply_and_voltage);
    RUN_TEST(test_known_supply_names_and_policy_remain_distinct);
    RUN_TEST(test_unknown_supply_fallback_selects_only_a_released_wake_input);
    return UNITY_END();
}
