#include "ble_fresh_flash_policy.h"

namespace BleFreshFlashPolicy {

uint32_t readStoredBondSchemaVersion(Preferences& prefs) {
    return prefs.getUInt(kBondSchemaVersionKey, 0);
}

bool hasBondSchemaMismatch(Preferences& prefs, const uint32_t currentSchemaVersion) {
    // Absence is the bootstrap state for devices that predate the dedicated
    // schema key. Schema 1 describes their existing compatible bond format,
    // so missing metadata must never be interpreted as an incompatible
    // schema transition.
    return prefs.isKey(kBondSchemaVersionKey) && readStoredBondSchemaVersion(prefs) != currentSchemaVersion;
}

bool storeBondSchemaVersion(Preferences& prefs, const uint32_t currentSchemaVersion) {
    prefs.putUInt(kBondSchemaVersionKey, currentSchemaVersion);
    return readStoredBondSchemaVersion(prefs) == currentSchemaVersion;
}

BondSchemaState prepareBondSchema(Preferences& prefs, const uint32_t currentSchemaVersion) {
    if (!prefs.isKey(kBondSchemaVersionKey)) {
        return storeBondSchemaVersion(prefs, currentSchemaVersion) ? BondSchemaState::Bootstrapped
                                                                   : BondSchemaState::BootstrapPending;
    }
    return hasBondSchemaMismatch(prefs, currentSchemaVersion) ? BondSchemaState::MigrationRequired
                                                               : BondSchemaState::Ready;
}

BondResetResult migrateBondSchema(Preferences& prefs, BackupBondsFn backupBonds, ClearBondsFn clearBonds,
                                  const uint32_t currentSchemaVersion) {
    BondResetResult result;
    if (!backupBonds || !clearBonds) {
        return result;
    }

    // Once a valid backup exists, persist that fact before clearing bonds. If
    // schema recording later fails or power is lost after the clear, the next
    // boot must reuse the pre-clear backup rather than replacing it with a
    // snapshot of the now-empty table.
    const bool readyBackupForTarget =
        prefs.getUInt(kBondMigrationBackupReadyKey, 0) == currentSchemaVersion;
    if (readyBackupForTarget) {
        result.reusedReadyBackup = true;
    } else {
        result.backedUpBondCount = backupBonds();
        if (result.backedUpBondCount < 0) {
            return result;
        }
        prefs.putUInt(kBondMigrationBackupReadyKey, currentSchemaVersion);
        if (prefs.getUInt(kBondMigrationBackupReadyKey, 0) != currentSchemaVersion) {
            return result;
        }
    }

    clearBonds();
    result.clearedBonds = true;
    result.recordedVersion = storeBondSchemaVersion(prefs, currentSchemaVersion);
    if (result.recordedVersion) {
        prefs.remove(kBondMigrationBackupReadyKey);
    }
    return result;
}

} // namespace BleFreshFlashPolicy
