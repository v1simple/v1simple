/**
 * Settings SD restore and validation paths.
 */

#include "settings_internals.h"
#include <nvs.h>
#include "settings_backup_doc.h"

namespace {

bool hasRestorableWifiStaSlots(const JsonDocument& doc) {
    if (!doc["wifiStaSlots"].is<JsonArrayConst>()) {
        return false;
    }
    for (JsonObjectConst slot : doc["wifiStaSlots"].as<JsonArrayConst>()) {
        const int index = slot["index"] | -1;
        if (index < 0 || index >= static_cast<int>(kWifiStaSlotCount) || !slot["ssid"].is<const char*>()) {
            continue;
        }
        if (sanitizeWifiClientSsidValue(slot["ssid"].as<String>()).length() > 0) {
            return true;
        }
    }
    return false;
}

} // namespace

bool shouldSkipProfileReferenceValidation(size_t availableProfileCount, bool hasConfiguredSlotReferences) {
    return availableProfileCount == 0 && hasConfiguredSlotReferences;
}

// --- Member methods: SD restore and validation ---

void SettingsManager::recoverCriticalSettingsAfterFullRestoreFailure(fs::FS* fs, bool hasSdBackup,
                                                                     const JsonDocument& backupDoc) {
    if (!storage_->isReady() || !storage_->isSDCard()) return;

    bool recovered = false;
    if (hasSdBackup) {
        Serial.println("[Settings] Attempting partial recovery from SD backup");
        applyBackupNetworkFields(backupDoc, settings_, *storage_, BackupRestoreScope::CriticalRecovery, false);
        applyBackupDisplayFields(backupDoc, settings_, BackupRestoreScope::CriticalRecovery);
        applyBackupAudioFields(backupDoc, settings_, BackupRestoreScope::CriticalRecovery);
        applyBackupProfileSlotFields(backupDoc, settings_, BackupRestoreScope::CriticalRecovery);
        applyBackupObdFields(backupDoc, settings_, BackupRestoreScope::CriticalRecovery);
        applyBackupAlpAndGpsFields(backupDoc, settings_);
        healBackupRestoreConflicts(settings_, "recovered");
        Serial.println("[Settings] Partial recovery from SD backup applied");
        recovered = true;
    }

    if (settings_.wifiClientSSID.length() == 0) {
        const WifiClientSecretPresence secret = readWifiClientSecretPresence(fs);
        if (secret.valid && secret.ssid.length() > 0) {
            settings_.wifiClientEnabled = true;
            settings_.wifiClientSSID = secret.ssid;
            settings_.ensureWifiStaSlotForLegacyAlias();
            Serial.println("[Settings] HEAL: recovered WiFi SSID from wifi_secret");
            recovered = true;
        }
    }

    if (recovered) {
        save();
        backupToSD();
    }
}

void SettingsManager::healWifiClientSettings(fs::FS* fs, bool hasSdBackup, const JsonDocument& backupDoc) {
    const WifiClientKeyPresence keyPresence = readWifiClientKeyPresence(getActiveNamespace().c_str());
    const bool legacySsidKeyRequired = settings_.wifiClientEnabled || settings_.hasConfiguredWifiStaSlot();
    const bool keysMissing = !keyPresence.enabledKeyPresent || (legacySsidKeyRequired && !keyPresence.ssidKeyPresent);
    const bool missingCurrentSsid = settings_.wifiClientSSID.length() == 0;

    if (keysMissing && !missingCurrentSsid) {
        settings_.wifiClientEnabled = true;
        Serial.println("[Settings] HEAL: repairing missing WiFi client keys from in-memory SSID");
        save();
        return;
    }
    if (!missingCurrentSsid) return;

    bool backupClientEnabled = false;
    const bool backupEnabledKnown = hasSdBackup && parseBoolVariant(backupDoc["wifiClientEnabled"], backupClientEnabled);
    const String backupSsid = hasSdBackup ? legacyWifiClientSsidFromBackupDoc(backupDoc) : "";
    const bool backupHasSsid = backupSsid.length() > 0;
    const bool backupHasSlots = hasSdBackup && hasRestorableWifiStaSlots(backupDoc);
    const WifiClientSecretPresence secret = readWifiClientSecretPresence(fs);
    const bool secretHasSsid = secret.valid && secret.ssid.length() > 0;

    String recoveredSsid;
    const char* recoveredFrom = "none";
    bool recoveredFromSlots = false;
    if (backupHasSlots && restoreWifiStaSlotsFromBackupDoc(backupDoc, settings_, *storage_, false)) {
        recoveredSsid = settings_.wifiClientSSID;
        recoveredFrom = "settings_backup_slots";
        recoveredFromSlots = true;
    } else if (backupHasSsid) {
        recoveredSsid = backupSsid;
        recoveredFrom = "settings_backup";
    } else if (secretHasSsid) {
        recoveredSsid = secret.ssid;
        recoveredFrom = "wifi_secret";
    }

    const bool shouldRecover = recoveredSsid.length() > 0 &&
                               (settings_.wifiClientEnabled || keysMissing ||
                                (backupEnabledKnown && backupClientEnabled) || secretHasSsid);
    if (shouldRecover) {
        settings_.wifiClientEnabled = true;
        if (!recoveredFromSlots) {
            settings_.wifiClientSSID = recoveredSsid;
            settings_.ensureWifiStaSlotForLegacyAlias();
        }
        Serial.printf("[Settings] HEAL: recovered WiFi client config from %s (keysMissing=%s)\n", recoveredFrom,
                      keysMissing ? "yes" : "no");
        if (backupHasSsid) {
            restoreWifiClientPasswordObfFromBackupDoc(backupDoc, settings_.wifiClientSSID);
            restoreLegacyStationPasswordFromBackupDoc(backupDoc, settings_.wifiClientSSID);
        }
        save();
    } else if (settings_.wifiClientEnabled) {
        settings_.wifiClientEnabled = false;
        Serial.println("[Settings] HEAL: wifiClientEnabled=true but no SSID anywhere — disabling");
        save();
    } else if (keysMissing) {
        Serial.println("[Settings] WARN: WiFi client keys missing and no SSID recovery source found");
    }
}

void SettingsManager::synchronizeSdBackup(bool hasSdBackup, const char* backupPath,
                                          const JsonDocument& backupDoc) {
    if (!hasSdBackup) {
        Serial.println("[Settings] No valid SD backup found; creating backup from current settings_");
        backupToSD();
        return;
    }
    const int version = backupDocumentVersion(backupDoc);
    const bool missingCoreFields = backupDoc["brightness"].isNull();
    const bool outOfSync = !backupAppearsInSyncWithNvs(backupDoc, settings_);
    if (version < SD_BACKUP_VERSION || missingCoreFields || outOfSync) {
        Serial.printf("[Settings] Refreshing SD backup schema (path=%s version=%d)\n",
                      backupPath ? backupPath : "(unknown)", version);
        if (outOfSync) Serial.println("[Settings] SD backup differs from healthy NVS; refreshing backup content");
        backupToSD();
    }
}

bool SettingsManager::checkAndRestoreFromSD() {
    // Check if NVS was erased (appears default) and backup exists on SD
    // This can be called after storage is mounted to retry the restore
    bool needsRestore = checkNeedsRestore();
    fs::FS* fs = nullptr;
    bool hasSdBackup = false;
    JsonDocument bestBackupDoc;
    const char* bestBackupPath = nullptr;
    if (storage_->isReady() && storage_->isSDCard()) {
        fs = storage_->getFilesystem();
        hasSdBackup = loadBestBackupDocument(fs, bestBackupDoc, &bestBackupPath, false);
    }

    if (needsRestore) {
        Serial.println("[Settings] Checking for SD backup restore...");
        if (restoreFromSD()) {
            Serial.println("[Settings] Restored settings_ from SD backup!");
            // Immediately re-emit backup in current schema after a successful restore.
            backupToSD();
            cleanupNamespacesIfNeeded(true);
            return true;
        }
        Serial.println("[Settings] Restore requested but no valid SD backup was applied");

        recoverCriticalSettingsAfterFullRestoreFailure(fs, hasSdBackup, bestBackupDoc);
    } else if (hasSdBackup) {
        // Keep user/NVS state authoritative unless corruption is detected.
        // Slot/profile healing is handled separately by validateProfileReferences().
        Serial.println("[Settings] NVS healthy; skipping automatic SD settings_ restore");
    }

    if (!needsRestore && storage_->isReady() && storage_->isSDCard()) {
        healWifiClientSettings(fs, hasSdBackup, bestBackupDoc);
    }
    if (!needsRestore && storage_->isReady() && storage_->isSDCard()) {
        synchronizeSdBackup(hasSdBackup, bestBackupPath, bestBackupDoc);
    }
    cleanupNamespacesIfNeeded(hasSdBackup);
    return false;
}

void SettingsManager::cleanupNamespacesIfNeeded(bool hasSdBackup) {
    nvs_stats_t stats;
    if (nvs_get_stats(NULL, &stats) != ESP_OK || stats.total_entries == 0) {
        return;
    }

    const uint32_t usedPct = (stats.used_entries * 100u) / stats.total_entries;
    const String activeNs = getActiveNamespace();
    const SettingsNamespaceCleanupPlan plan = buildSettingsNamespaceCleanupPlan(usedPct, activeNs, hasSdBackup);

    if (!plan.shouldCleanup) {
        if (usedPct > 80) {
            Serial.printf("[Settings] NVS high usage (%lu%%); deferring cleanup (active=%s backup=%s)\n",
                          static_cast<unsigned long>(usedPct), activeNs.c_str(), hasSdBackup ? "yes" : "no");
        }
        return;
    }

    auto clearNamespaceIfPresent = [](const char* ns, const char* label) {
        if (!ns || ns[0] == '\0' || namespaceHealthScore(ns) <= 0) {
            return;
        }
        Preferences prefs;
        if (prefs.begin(ns, false)) {
            prefs.clear();
            prefs.end();
            Serial.printf("[Settings] Cleared %s namespace %s\n", label, ns);
        }
    };

    Serial.printf("[Settings] NVS high usage (%lu%%); cleaning stale namespaces after active resolution (active=%s)\n",
                  static_cast<unsigned long>(usedPct), activeNs.c_str());
    clearNamespaceIfPresent(plan.inactiveNamespace, "inactive");
    if (plan.clearLegacyNamespace) {
        clearNamespaceIfPresent(SETTINGS_NS_LEGACY, "legacy");
    }
}

bool SettingsManager::checkNeedsRestore() {
    // Check if NVS was likely wiped by looking for the settings version marker
    // If settingsVer is missing (defaults to 1, triggers migration message),
    // that's a strong indicator NVS was erased during a partition table change
    //
    // We use a dedicated "nvsValid" marker that's only set after a successful save
    // If this marker is missing but an SD backup exists, we should restore

    String activeNs = getActiveNamespace();
    Preferences checkPrefs;
    if (!checkPrefs.begin(activeNs.c_str(), true)) {
        // Can't even open the namespace - definitely needs restore
        markRestorePending("active NVS namespace could not be opened");
        return true;
    }

    // Check for our validity marker - set to current version after successful save
    int nvsMarker = checkPrefs.getInt(kNvsValid, 0);
    int settingsVer = checkPrefs.getInt(kNvsSettingsVer, 0);
    const bool persistedRestorePending = checkPrefs.getBool(kNvsRestorePending, false);
    bool missingCriticalKey = false;
    // These keys exist in all modern schemas and should never disappear in a healthy namespace.
    static constexpr const char* kCriticalKeys[] = {kNvsProxyBle, kNvsProxyName, kNvsBrightness, kNvsAutoPush};
    for (const char* key : kCriticalKeys) {
        if (!checkPrefs.isKey(key)) {
            missingCriticalKey = true;
            Serial.printf("[Settings] Missing critical key '%s' in active namespace\n", key);
        }
    }
    checkPrefs.end();

    if (persistedRestorePending || restorePending_) {
        markRestorePending("restore-pending marker set");
        return true;
    }

    // If neither marker exists, NVS was likely wiped
    if (nvsMarker == 0 && settingsVer == 0) {
        Serial.println("[Settings] NVS appears empty (no version markers)");
        markRestorePending("NVS empty before SD restore");
        return true;
    }

    // Also check if this looks like a v1-format namespace that was never upgraded.
    // The brightness==200 clause was removed because it caused false negatives:
    // any device legitimately running non-default brightness at settings version <=1
    // would have had a valid restore silently skipped.  nvsMarker==0 + settingsVer<=1
    // is the correct and sufficient signal.
    if (nvsMarker == 0 && settingsVer <= 1) {
        Serial.println("[Settings] NVS appears default (v1 migration + default brightness)");
        markRestorePending("legacy/default NVS before SD restore");
        return true;
    }

    // Any missing critical key means this namespace is not trustworthy,
    // regardless of marker/version combinations.
    if (missingCriticalKey) {
        Serial.println("[Settings] NVS appears partial/corrupt (critical keys missing)");
        markRestorePending("critical NVS keys missing");
        return true;
    }

    // nvsValid means a full write completed; tolerate legacy/missing settingsVer
    // to avoid clobbering valid user settings with an older SD backup.

    // Detect incomplete writes: settingsVer is the FIRST key written and
    // nvsValid is the LAST.  If settingsVer exists but nvsValid does not,
    // the namespace was only partially written (crash/reset mid-save).
    if (nvsMarker == 0 && settingsVer >= SETTINGS_VERSION) {
        Serial.println("[Settings] NVS appears incomplete (settingsVer present but nvsValid missing)");
        markRestorePending("incomplete NVS write detected");
        return true;
    }

    return false;
}

// Restore ALL settings from SD card

bool SettingsManager::restoreFromSD() {
    if (!storage_->isReady() || !storage_->isSDCard()) {
        return false;
    }

    // Acquire SD mutex to protect file I/O
    StorageManager::SDLockBlocking sdLock(storage_->getSDMutex());
    if (!sdLock) {
        Serial.println("[Settings] Failed to acquire SD mutex for restore");
        return false;
    }

    fs::FS* fs = storage_->getFilesystem();
    if (!fs)
        return false;

    const char* backupPath = nullptr;
    JsonDocument doc;
    if (!loadBestBackupDocument(fs, doc, &backupPath, true)) {
        backupPath = nullptr;
    }

    if (!backupPath) {
        Serial.println("[Settings] No valid SD backup found");
        return false;
    }

    Serial.printf("[Settings] Using backup file: %s\n", backupPath);

    int backupVersion = doc["_version"] | doc["version"] | 1;
    Serial.printf("[Settings] Restoring from SD backup (version %d)\n", backupVersion);
    bool backupAutoPush = false;
    const bool hasAutoPush = parseBoolVariant(doc["autoPushEnabled"], backupAutoPush);
    const bool backupSlot0Configured =
        doc["slot0ProfileName"].is<const char*>() && doc["slot0ProfileName"].as<const char*>()[0] != '\0';
    const int backupSlot0Mode = doc["slot0Mode"].is<int>() ? doc["slot0Mode"].as<int>() : -1;
    Serial.printf("[Settings] Backup fields: autoPush=%s slot0ProfileConfigured=%s slot0Mode=%d\n",
                  hasAutoPush ? (backupAutoPush ? "true" : "false") : "missing", backupSlot0Configured ? "yes" : "no",
                  backupSlot0Mode);

    const SettingsBackupApplyResult applyResult = applyBackupDocument(doc, false);
    if (!applyResult.success) {
        return false;
    }
    Serial.printf("[Settings] Restored modes from backup: slot0Mode=%d (in json: %s), slot1Mode=%d (in json: %s), "
                  "slot2Mode=%d (in json: %s)\n",
                  settings_.slot0_default.mode, doc["slot0Mode"].is<int>() ? "yes" : "NO", settings_.slot1_highway.mode,
                  doc["slot1Mode"].is<int>() ? "yes" : "NO", settings_.slot2_comfort.mode,
                  doc["slot2Mode"].is<int>() ? "yes" : "NO");
    Serial.printf("[Settings] ✅ Full restore from SD backup complete (%d profiles)\n", applyResult.profilesRestored);
    return true;
}

void SettingsManager::validateProfileReferences(V1ProfileManager& profileMgr) {
    if (!profileMgr.isReady()) {
        Serial.println("[Settings] Profile manager not ready; skipping profile reference validation");
        return;
    }

    const bool hasConfiguredSlotReferences = settings_.slot0_default.profileName.length() > 0 ||
                                             settings_.slot1_highway.profileName.length() > 0 ||
                                             settings_.slot2_comfort.profileName.length() > 0;
    const size_t availableProfileCount = profileMgr.listProfiles().size();
    if (shouldSkipProfileReferenceValidation(availableProfileCount, hasConfiguredSlotReferences)) {
        Serial.println("[Settings] Profile catalog empty; preserving slot profile references");
        return;
    }

    // Validate that profile names in auto-push slots actually exist
    // If not, clear them to prevent repeated "file not found" errors
    bool needsSave = false;

    auto validateSlot = [&](AutoPushSlot& slot, const char* slotName) {
        if (slot.profileName.length() > 0) {
            V1Profile testProfile;
            if (!profileMgr.loadProfile(slot.profileName, testProfile)) {
                Serial.printf("[Settings] WARN: Profile reference for %s does not exist - clearing reference\n",
                              slotName);
                slot.profileName = "";
                needsSave = true;
            } else {
                Serial.printf("[Settings] Profile reference for %s validated OK\n", slotName);
            }
        }
    };

    validateSlot(settings_.slot0_default, "Slot 0 (Default)");
    validateSlot(settings_.slot1_highway, "Slot 1 (Highway)");
    validateSlot(settings_.slot2_comfort, "Slot 2 (Comfort)");

    if (needsSave) {
        if (persistSettingsAtomically()) {
            noteNvsCommitWithoutBackupIntent();
            Serial.println("[Settings] Cleared invalid profile references and saved");
        } else {
            Serial.println("[Settings] ERROR: Failed to persist cleared profile references");
        }
    }

    // No additional side effects needed beyond clearing invalid references.
}
