#include <unity.h>

#include "../mocks/Arduino.h"
#include "../../include/settings_internals.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

void setUp(void) {}
void tearDown(void) {}

void test_production_namespace_ids_match_expected_literals() {
    TEST_ASSERT_EQUAL_STRING("v1settingsA", SETTINGS_NS_A);
    TEST_ASSERT_EQUAL_STRING("v1settingsB", SETTINGS_NS_B);
    TEST_ASSERT_EQUAL_STRING("v1settingsMeta", SETTINGS_NS_META);
    TEST_ASSERT_EQUAL_STRING("v1settings", SETTINGS_NS_LEGACY);
}

void test_legacy_only_namespace_defers_cleanup_even_when_usage_is_high() {
    const SettingsNamespaceCleanupPlan plan =
        buildSettingsNamespaceCleanupPlan(95, String(SETTINGS_NS_LEGACY), false);

    TEST_ASSERT_FALSE(plan.shouldCleanup);
    TEST_ASSERT_NULL(plan.inactiveNamespace);
    TEST_ASSERT_FALSE(plan.clearLegacyNamespace);
}

void test_active_a_cleans_inactive_b_without_clearing_legacy_when_no_sd_backup() {
    const SettingsNamespaceCleanupPlan plan =
        buildSettingsNamespaceCleanupPlan(95, String(SETTINGS_NS_A), false);

    TEST_ASSERT_TRUE(plan.shouldCleanup);
    TEST_ASSERT_EQUAL_STRING(SETTINGS_NS_B, plan.inactiveNamespace);
    TEST_ASSERT_FALSE(plan.clearLegacyNamespace);
}

void test_active_b_cleans_inactive_a_and_legacy_when_sd_backup_exists() {
    const SettingsNamespaceCleanupPlan plan =
        buildSettingsNamespaceCleanupPlan(95, String(SETTINGS_NS_B), true);

    TEST_ASSERT_TRUE(plan.shouldCleanup);
    TEST_ASSERT_EQUAL_STRING(SETTINGS_NS_A, plan.inactiveNamespace);
    TEST_ASSERT_TRUE(plan.clearLegacyNamespace);
}

void test_missing_meta_resolution_defers_cleanup_when_active_namespace_is_unknown() {
    const SettingsNamespaceCleanupPlan plan =
        buildSettingsNamespaceCleanupPlan(95, String(""), false);

    TEST_ASSERT_FALSE(plan.shouldCleanup);
    TEST_ASSERT_NULL(plan.inactiveNamespace);
    TEST_ASSERT_FALSE(plan.clearLegacyNamespace);
}

void test_low_usage_never_triggers_cleanup() {
    const SettingsNamespaceCleanupPlan plan =
        buildSettingsNamespaceCleanupPlan(80, String(SETTINGS_NS_A), true);

    TEST_ASSERT_FALSE(plan.shouldCleanup);
    TEST_ASSERT_NULL(plan.inactiveNamespace);
    TEST_ASSERT_FALSE(plan.clearLegacyNamespace);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_production_namespace_ids_match_expected_literals);
    RUN_TEST(test_legacy_only_namespace_defers_cleanup_even_when_usage_is_high);
    RUN_TEST(test_active_a_cleans_inactive_b_without_clearing_legacy_when_no_sd_backup);
    RUN_TEST(test_active_b_cleans_inactive_a_and_legacy_when_sd_backup_exists);
    RUN_TEST(test_missing_meta_resolution_defers_cleanup_when_active_namespace_is_unknown);
    RUN_TEST(test_low_usage_never_triggers_cleanup);
    return UNITY_END();
}
