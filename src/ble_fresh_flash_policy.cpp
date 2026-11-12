#include "ble_fresh_flash_policy.h"
#include <nvs.h>
#include <array>

namespace BleFreshFlashPolicy {
namespace {

esp_err_t readMetadata(const char* key, uint32_t& value) {
    nvs_handle_t handle = 0;
    const esp_err_t opened = nvs_open(kNamespace, NVS_READONLY, &handle);
    if (opened != ESP_OK) return opened;
    const esp_err_t read = nvs_get_u32(handle, key, &value);
    nvs_close(handle);
    return read;
}

bool storeMetadata(Preferences& prefs, const char* key, uint32_t value) {
    uint32_t recorded = 0;
    return prefs.putUInt(key, value) == sizeof(value) &&
           readMetadata(key, recorded) == ESP_OK && recorded == value;
}

using BackupMarker = std::array<uint32_t, 3>;

BackupMarker backupMarker(uint32_t version, uint64_t identity) {
    return {version, static_cast<uint32_t>(identity), static_cast<uint32_t>(identity >> 32)};
}

esp_err_t readBackupMarker(BackupMarker& marker) {
    nvs_handle_t handle = 0;
    const esp_err_t opened = nvs_open(kNamespace, NVS_READONLY, &handle);
    if (opened != ESP_OK) return opened;
    size_t size = sizeof(marker);
    const esp_err_t read = nvs_get_blob(handle, kBondMigrationBackupIdentityKey, marker.data(), &size);
    nvs_close(handle);
    return read == ESP_OK && size != sizeof(marker) ? ESP_ERR_NVS_INVALID_LENGTH : read;
}

bool storeBackupMarker(Preferences& prefs, const BackupMarker& marker) {
    BackupMarker recorded{};
    return prefs.putBytes(kBondMigrationBackupIdentityKey, marker.data(), sizeof(marker)) == sizeof(marker) &&
           readBackupMarker(recorded) == ESP_OK && recorded == marker;
}

} // namespace

bool storeBondSchemaVersion(Preferences& prefs, const uint32_t currentSchemaVersion) {
    return storeMetadata(prefs, kBondSchemaVersionKey, currentSchemaVersion);
}

BondSchemaState prepareBondSchema(Preferences& prefs, const uint32_t currentSchemaVersion) {
    uint32_t stored = 0;
    const esp_err_t status = readMetadata(kBondSchemaVersionKey, stored);
    BackupMarker marker{};
    const esp_err_t markerStatus = readBackupMarker(marker);
    if (markerStatus != ESP_ERR_NVS_NOT_FOUND) {
        if (status != ESP_OK || markerStatus != ESP_OK || marker[0] != currentSchemaVersion)
            return BondSchemaState::Unavailable;
        return stored == currentSchemaVersion ? BondSchemaState::MigrationRestoreRequired
                                              : BondSchemaState::MigrationResumeRequired;
    }
    if (status == ESP_ERR_NVS_NOT_FOUND) {
        // Unversioned devices already use the compatible bond format.
        return storeBondSchemaVersion(prefs, currentSchemaVersion) ? BondSchemaState::Bootstrapped
                                                                   : BondSchemaState::BootstrapPending;
    }
    if (status != ESP_OK) return BondSchemaState::Unavailable;
    return stored == currentSchemaVersion ? BondSchemaState::Ready : BondSchemaState::MigrationRequired;
}

BondResetResult migrateBondSchema(Preferences& prefs, BackupBondsFn backupBonds, ClearBondsFn clearBonds,
                                  ReadBackupIdentityFn readBackupIdentity, void* backupContext,
                                  const uint32_t currentSchemaVersion) {
    BondResetResult result;
    if (!backupBonds || !clearBonds || !readBackupIdentity) {
        return result;
    }

    // Once a valid backup exists, persist that fact before clearing bonds. If
    // schema recording later fails or power is lost after the clear, the next
    // boot must reuse the pre-clear backup rather than replacing it with a
    // snapshot of the now-empty table.
    BackupMarker marker{};
    const esp_err_t markerStatus = readBackupMarker(marker);
    if (markerStatus != ESP_ERR_NVS_NOT_FOUND) {
        // A partial clear can leave an incomplete live bond table. Only the
        // original archive can authorize a retry, never a fresh snapshot.
        result.migrationInterrupted = true;
        if (markerStatus != ESP_OK || marker[0] != currentSchemaVersion) return result;
        const auto identity = readBackupIdentity(backupContext);
        if (!identity || marker != backupMarker(currentSchemaVersion, *identity)) return result;
        result.reusedReadyBackup = true;
    } else {
        uint32_t legacyVersion = 0;
        if (readMetadata(kBondMigrationBackupReadyKey, legacyVersion) != ESP_ERR_NVS_NOT_FOUND) {
            // The former schema-only marker does not identify the archive.
            result.migrationInterrupted = true;
            return result;
        }
        uint64_t capturedIdentity = 0;
        result.backedUpBondCount = backupBonds(backupContext, capturedIdentity);
        if (result.backedUpBondCount < 0) {
            return result;
        }
        const auto identity = readBackupIdentity(backupContext);
        if (!identity || *identity != capturedIdentity) return result;
        // A failed readback can still leave a durable marker. Freeze backup
        // replacement until a later boot resolves that uncertainty.
        result.migrationInterrupted = true;
        if (!storeBackupMarker(prefs, backupMarker(currentSchemaVersion, capturedIdentity))) return result;
    }

    result.migrationInterrupted = true;
    result.clearedBonds = clearBonds();
    if (!result.clearedBonds) return result;
    result.recordedVersion = storeBondSchemaVersion(prefs, currentSchemaVersion);
    result.migrationInterrupted = !result.recordedVersion;
    return result;
}

bool finishBondSchemaMigration(Preferences& prefs, RestoreBackupFn restoreBackup, void* backupContext,
                               const uint32_t currentSchemaVersion) {
    BackupMarker marker{};
    if (!restoreBackup || readBackupMarker(marker) != ESP_OK || marker[0] != currentSchemaVersion)
        return false;
    const uint64_t identity = static_cast<uint64_t>(marker[2]) << 32 | marker[1];
    return restoreBackup(backupContext, identity) && prefs.remove(kBondMigrationBackupIdentityKey);
}

} // namespace BleFreshFlashPolicy
