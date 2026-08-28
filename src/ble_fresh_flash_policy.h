#pragma once

#include <Arduino.h>
#include <Preferences.h>

namespace BleFreshFlashPolicy {

constexpr const char* kNamespace = "ble_state";
constexpr const char* kBondSchemaVersionKey = "bondSchema";
constexpr const char* kBondMigrationBackupReadyKey = "bondMigReady";
constexpr uint32_t kCurrentBondSchemaVersion = 1;

using BackupBondsFn = int (*)();
using ClearBondsFn = void (*)();

struct BondResetResult {
    int backedUpBondCount = -1;
    bool reusedReadyBackup = false;
    bool clearedBonds = false;
    bool recordedVersion = false;
};

enum class BondSchemaState : uint8_t {
    Ready,
    Bootstrapped,
    BootstrapPending,
    MigrationRequired,
};

uint32_t readStoredBondSchemaVersion(Preferences& prefs);
BondSchemaState prepareBondSchema(Preferences& prefs,
                                  uint32_t currentSchemaVersion = kCurrentBondSchemaVersion);
bool hasBondSchemaMismatch(Preferences& prefs, uint32_t currentSchemaVersion = kCurrentBondSchemaVersion);
bool storeBondSchemaVersion(Preferences& prefs, uint32_t currentSchemaVersion = kCurrentBondSchemaVersion);
BondResetResult migrateBondSchema(Preferences& prefs, BackupBondsFn backupBonds, ClearBondsFn clearBonds,
                                  uint32_t currentSchemaVersion = kCurrentBondSchemaVersion);

} // namespace BleFreshFlashPolicy
