#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include <optional>

namespace BleFreshFlashPolicy {

constexpr const char* kNamespace = "ble_state";
constexpr const char* kBondSchemaVersionKey = "bondSchema";
constexpr const char* kBondMigrationBackupReadyKey = "bondMigReady";
constexpr const char* kBondMigrationBackupIdentityKey = "bondMigBackup";
constexpr uint32_t kCurrentBondSchemaVersion = 1;

using BackupBondsFn = int (*)(void*, uint64_t&);
using ClearBondsFn = bool (*)();
using ReadBackupIdentityFn = std::optional<uint64_t> (*)(void*);
using RestoreBackupFn = bool (*)(void*, uint64_t);

struct BondResetResult {
    int backedUpBondCount = -1;
    bool reusedReadyBackup = false;
    bool clearedBonds = false;
    bool recordedVersion = false;
    bool migrationInterrupted = false;
};

enum class BondSchemaState : uint8_t {
    Ready,
    Bootstrapped,
    BootstrapPending,
    MigrationRequired,
    MigrationResumeRequired,
    MigrationRestoreRequired,
    Unavailable,
};

BondSchemaState prepareBondSchema(Preferences& prefs,
                                  uint32_t currentSchemaVersion = kCurrentBondSchemaVersion);
bool storeBondSchemaVersion(Preferences& prefs, uint32_t currentSchemaVersion = kCurrentBondSchemaVersion);
BondResetResult migrateBondSchema(Preferences& prefs, BackupBondsFn backupBonds, ClearBondsFn clearBonds,
                                  ReadBackupIdentityFn readBackupIdentity, void* backupContext,
                                  uint32_t currentSchemaVersion = kCurrentBondSchemaVersion);
bool finishBondSchemaMigration(Preferences& prefs, RestoreBackupFn restoreBackup, void* backupContext,
                               uint32_t currentSchemaVersion = kCurrentBondSchemaVersion);

} // namespace BleFreshFlashPolicy
