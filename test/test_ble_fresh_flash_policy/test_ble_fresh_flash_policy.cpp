#include <unity.h>

#include "../mocks/Arduino.h"
#include "../mocks/Preferences.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#include "../../src/ble_fresh_flash_policy.cpp"

namespace {
int backupResult = 0;
int backupCalls = 0;
int clearCalls = 0;

int backupBonds() {
    ++backupCalls;
    return backupResult;
}
void clearBonds() { ++clearCalls; }
}

void setUp() {
    mock_preferences::reset();
    backupResult = 0;
    backupCalls = 0;
    clearCalls = 0;
}

void tearDown() {}

void test_marketing_version_is_not_part_of_bond_schema_policy() {
    Preferences prefs;
    TEST_ASSERT_TRUE(prefs.begin(BleFreshFlashPolicy::kNamespace, false));
    TEST_ASSERT_TRUE(BleFreshFlashPolicy::storeBondSchemaVersion(prefs));

    // There is deliberately no firmware-version input: 2.0.3 -> 2.0.4 does
    // not create a bond migration when the storage schema remains compatible.
    TEST_ASSERT_FALSE(BleFreshFlashPolicy::hasBondSchemaMismatch(prefs));
}

void test_legacy_firmware_version_only_state_bootstraps_without_clearing_bonds() {
    Preferences prefs;
    TEST_ASSERT_TRUE(prefs.begin(BleFreshFlashPolicy::kNamespace, false));
    prefs.putString("fwVersion", "2.0.2");

    TEST_ASSERT_EQUAL_INT(static_cast<int>(BleFreshFlashPolicy::BondSchemaState::Bootstrapped),
                          static_cast<int>(BleFreshFlashPolicy::prepareBondSchema(prefs)));
    TEST_ASSERT_FALSE(BleFreshFlashPolicy::hasBondSchemaMismatch(prefs));
    TEST_ASSERT_EQUAL_INT(0, clearCalls);
}

void test_failed_bootstrap_record_is_non_destructive_and_retries_later() {
    Preferences prefs;
    TEST_ASSERT_TRUE(prefs.begin(BleFreshFlashPolicy::kNamespace, false));
    mock_preferences::set_fail_writes_for_key(BleFreshFlashPolicy::kBondSchemaVersionKey);

    TEST_ASSERT_EQUAL_INT(static_cast<int>(BleFreshFlashPolicy::BondSchemaState::BootstrapPending),
                          static_cast<int>(BleFreshFlashPolicy::prepareBondSchema(prefs)));
    TEST_ASSERT_FALSE(BleFreshFlashPolicy::hasBondSchemaMismatch(prefs));
    TEST_ASSERT_EQUAL_INT(0, clearCalls);

    mock_preferences::set_fail_writes_for_key(nullptr);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(BleFreshFlashPolicy::BondSchemaState::Bootstrapped),
                          static_cast<int>(BleFreshFlashPolicy::prepareBondSchema(prefs)));
}

void test_successful_backup_allows_clear_and_records_schema() {
    Preferences prefs;
    TEST_ASSERT_TRUE(prefs.begin(BleFreshFlashPolicy::kNamespace, false));
    TEST_ASSERT_TRUE(BleFreshFlashPolicy::storeBondSchemaVersion(prefs, 1));
    backupResult = 2;

    TEST_ASSERT_EQUAL_INT(static_cast<int>(BleFreshFlashPolicy::BondSchemaState::MigrationRequired),
                          static_cast<int>(BleFreshFlashPolicy::prepareBondSchema(prefs, 2)));
    const auto result = BleFreshFlashPolicy::migrateBondSchema(prefs, backupBonds, clearBonds, 2);

    TEST_ASSERT_EQUAL_INT(2, result.backedUpBondCount);
    TEST_ASSERT_FALSE(result.reusedReadyBackup);
    TEST_ASSERT_EQUAL_INT(1, backupCalls);
    TEST_ASSERT_TRUE(result.clearedBonds);
    TEST_ASSERT_TRUE(result.recordedVersion);
    TEST_ASSERT_EQUAL_INT(1, clearCalls);
    TEST_ASSERT_FALSE(BleFreshFlashPolicy::hasBondSchemaMismatch(prefs, 2));
}

void test_failed_backup_prevents_destructive_clear_and_leaves_migration_pending() {
    Preferences prefs;
    TEST_ASSERT_TRUE(prefs.begin(BleFreshFlashPolicy::kNamespace, false));
    TEST_ASSERT_TRUE(BleFreshFlashPolicy::storeBondSchemaVersion(prefs, 1));
    backupResult = -1;

    const auto result = BleFreshFlashPolicy::migrateBondSchema(prefs, backupBonds, clearBonds, 2);

    TEST_ASSERT_EQUAL_INT(-1, result.backedUpBondCount);
    TEST_ASSERT_FALSE(result.clearedBonds);
    TEST_ASSERT_FALSE(result.recordedVersion);
    TEST_ASSERT_EQUAL_INT(0, clearCalls);
    TEST_ASSERT_EQUAL_INT(1, backupCalls);
    TEST_ASSERT_TRUE(BleFreshFlashPolicy::hasBondSchemaMismatch(prefs, 2));
}

void test_failed_backup_ready_marker_prevents_destructive_clear() {
    Preferences prefs;
    TEST_ASSERT_TRUE(prefs.begin(BleFreshFlashPolicy::kNamespace, false));
    TEST_ASSERT_TRUE(BleFreshFlashPolicy::storeBondSchemaVersion(prefs, 1));
    backupResult = 2;
    mock_preferences::set_fail_writes_for_key(BleFreshFlashPolicy::kBondMigrationBackupReadyKey);

    const auto result = BleFreshFlashPolicy::migrateBondSchema(prefs, backupBonds, clearBonds, 2);

    TEST_ASSERT_EQUAL_INT(2, result.backedUpBondCount);
    TEST_ASSERT_FALSE(result.clearedBonds);
    TEST_ASSERT_FALSE(result.recordedVersion);
    TEST_ASSERT_EQUAL_INT(1, backupCalls);
    TEST_ASSERT_EQUAL_INT(0, clearCalls);
    TEST_ASSERT_TRUE(BleFreshFlashPolicy::hasBondSchemaMismatch(prefs, 2));
}

void test_failed_schema_record_retry_preserves_pre_clear_backup() {
    Preferences prefs;
    TEST_ASSERT_TRUE(prefs.begin(BleFreshFlashPolicy::kNamespace, false));
    TEST_ASSERT_TRUE(BleFreshFlashPolicy::storeBondSchemaVersion(prefs, 1));
    backupResult = 1;
    mock_preferences::set_fail_writes_for_key(BleFreshFlashPolicy::kBondSchemaVersionKey);

    const auto result = BleFreshFlashPolicy::migrateBondSchema(prefs, backupBonds, clearBonds, 2);

    TEST_ASSERT_FALSE(result.recordedVersion);
    TEST_ASSERT_TRUE(result.clearedBonds);
    TEST_ASSERT_EQUAL_INT(1, clearCalls);
    TEST_ASSERT_EQUAL_INT(1, backupCalls);
    TEST_ASSERT_EQUAL_UINT32(2, prefs.getUInt(BleFreshFlashPolicy::kBondMigrationBackupReadyKey, 0));
    TEST_ASSERT_TRUE(BleFreshFlashPolicy::hasBondSchemaMismatch(prefs, 2));

    // Modeled reboot: writes recover, but the live bond table is now empty.
    // The durable ready marker must prevent a second backup callback from
    // overwriting the only pre-clear backup with that empty state.
    backupResult = 0;
    mock_preferences::set_fail_writes_for_key(nullptr);
    const auto retry = BleFreshFlashPolicy::migrateBondSchema(prefs, backupBonds, clearBonds, 2);

    TEST_ASSERT_TRUE(retry.reusedReadyBackup);
    TEST_ASSERT_EQUAL_INT(1, backupCalls);
    TEST_ASSERT_EQUAL_INT(2, clearCalls);
    TEST_ASSERT_TRUE(retry.recordedVersion);
    TEST_ASSERT_FALSE(BleFreshFlashPolicy::hasBondSchemaMismatch(prefs, 2));
    TEST_ASSERT_FALSE(prefs.isKey(BleFreshFlashPolicy::kBondMigrationBackupReadyKey));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_marketing_version_is_not_part_of_bond_schema_policy);
    RUN_TEST(test_legacy_firmware_version_only_state_bootstraps_without_clearing_bonds);
    RUN_TEST(test_failed_bootstrap_record_is_non_destructive_and_retries_later);
    RUN_TEST(test_successful_backup_allows_clear_and_records_schema);
    RUN_TEST(test_failed_backup_prevents_destructive_clear_and_leaves_migration_pending);
    RUN_TEST(test_failed_backup_ready_marker_prevents_destructive_clear);
    RUN_TEST(test_failed_schema_record_retry_preserves_pre_clear_backup);
    return UNITY_END();
}
