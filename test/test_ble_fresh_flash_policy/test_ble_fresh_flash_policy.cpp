#include <unity.h>

#include "../mocks/Arduino.h"
#include "../mocks/Preferences.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#include "../../src/ble_fresh_flash_policy.h"
#include "../../src/ble_fresh_flash_policy.cpp"

void setUp() {
    mock_preferences::reset();
}

void tearDown() {}

namespace {

int g_backupCallCount = 0;
int g_clearCallCount = 0;
int g_backupReturnValue = 0;
String g_callOrder;

int recordBackupCall() {
    ++g_backupCallCount;
    g_callOrder += "backup>";
    return g_backupReturnValue;
}

void recordClearCall() {
    ++g_clearCallCount;
    g_callOrder += "clear>";
}

}  // namespace

void test_missing_version_is_treated_as_mismatch() {
    Preferences prefs;
    TEST_ASSERT_TRUE(prefs.begin(BleFreshFlashPolicy::kNamespace, false));

    TEST_ASSERT_TRUE(BleFreshFlashPolicy::hasFirmwareVersionMismatch(prefs, "4.0.0-dev"));

    prefs.end();
}

void test_matching_version_is_not_a_mismatch() {
    Preferences prefs;
    TEST_ASSERT_TRUE(prefs.begin(BleFreshFlashPolicy::kNamespace, false));
    TEST_ASSERT_TRUE(BleFreshFlashPolicy::storeFirmwareVersion(prefs, "4.0.0-dev"));

    TEST_ASSERT_FALSE(BleFreshFlashPolicy::hasFirmwareVersionMismatch(prefs, "4.0.0-dev"));

    prefs.end();
}

void test_store_version_updates_persisted_value() {
    Preferences prefs;
    TEST_ASSERT_TRUE(prefs.begin(BleFreshFlashPolicy::kNamespace, false));

    TEST_ASSERT_TRUE(BleFreshFlashPolicy::storeFirmwareVersion(prefs, "4.0.1"));
    TEST_ASSERT_EQUAL_STRING("4.0.1", BleFreshFlashPolicy::readStoredFirmwareVersion(prefs).c_str());
    TEST_ASSERT_TRUE(BleFreshFlashPolicy::hasFirmwareVersionMismatch(prefs, "4.0.2"));
    TEST_ASSERT_FALSE(BleFreshFlashPolicy::hasFirmwareVersionMismatch(prefs, "4.0.1"));

    prefs.end();
}

void test_null_current_version_normalizes_to_empty_string() {
    Preferences prefs;
    TEST_ASSERT_TRUE(prefs.begin(BleFreshFlashPolicy::kNamespace, false));

    TEST_ASSERT_TRUE(BleFreshFlashPolicy::storeFirmwareVersion(prefs, nullptr));
    TEST_ASSERT_EQUAL_STRING("", BleFreshFlashPolicy::readStoredFirmwareVersion(prefs).c_str());
    TEST_ASSERT_FALSE(BleFreshFlashPolicy::hasFirmwareVersionMismatch(prefs, nullptr));
    TEST_ASSERT_TRUE(BleFreshFlashPolicy::hasFirmwareVersionMismatch(prefs, "4.0.0-dev"));

    prefs.end();
}

void test_reset_bonds_for_version_runs_backup_clear_and_version_stamp() {
    Preferences prefs;
    TEST_ASSERT_TRUE(prefs.begin(BleFreshFlashPolicy::kNamespace, false));
    g_backupCallCount = 0;
    g_clearCallCount = 0;
    g_backupReturnValue = 3;
    g_callOrder = "";

    const BleFreshFlashPolicy::BondResetResult result =
        BleFreshFlashPolicy::resetBondsForFirmwareVersion(
            prefs,
            "4.0.0-dev",
            recordBackupCall,
            recordClearCall);

    TEST_ASSERT_EQUAL_INT(3, result.backedUpBondCount);
    TEST_ASSERT_TRUE(result.clearedBonds);
    TEST_ASSERT_TRUE(result.recordedVersion);
    TEST_ASSERT_EQUAL_INT(1, g_backupCallCount);
    TEST_ASSERT_EQUAL_INT(1, g_clearCallCount);
    TEST_ASSERT_EQUAL_STRING("backup>clear>", g_callOrder.c_str());
    TEST_ASSERT_EQUAL_STRING("4.0.0-dev", BleFreshFlashPolicy::readStoredFirmwareVersion(prefs).c_str());

    prefs.end();
}

void test_reset_bonds_for_version_tolerates_missing_callbacks() {
    Preferences prefs;
    TEST_ASSERT_TRUE(prefs.begin(BleFreshFlashPolicy::kNamespace, false));

    const BleFreshFlashPolicy::BondResetResult result =
        BleFreshFlashPolicy::resetBondsForFirmwareVersion(
            prefs,
            "4.0.1",
            nullptr,
            nullptr);

    TEST_ASSERT_EQUAL_INT(-1, result.backedUpBondCount);
    TEST_ASSERT_FALSE(result.clearedBonds);
    TEST_ASSERT_TRUE(result.recordedVersion);
    TEST_ASSERT_EQUAL_STRING("4.0.1", BleFreshFlashPolicy::readStoredFirmwareVersion(prefs).c_str());

    prefs.end();
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_missing_version_is_treated_as_mismatch);
    RUN_TEST(test_matching_version_is_not_a_mismatch);
    RUN_TEST(test_store_version_updates_persisted_value);
    RUN_TEST(test_null_current_version_normalizes_to_empty_string);
    RUN_TEST(test_reset_bonds_for_version_runs_backup_clear_and_version_stamp);
    RUN_TEST(test_reset_bonds_for_version_tolerates_missing_callbacks);
    return UNITY_END();
}
