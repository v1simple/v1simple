/**
 * Settings SD restore and validation paths.
 */

#include "settings_internals.h"
#include <nvs.h>
#include "psram_json_document.h"
#include "settings_backup_doc.h"
#include "v1_settings_json.h"

namespace {

enum class ProfileRecoveryStatus : uint8_t { Restored, NotFound, Invalid, Unavailable };

ProfileRecoveryStatus restoreProfileEntryFromBackup(const JsonDocument& backup, const String& canonicalName,
                                                     V1ProfileManager& profiles) {
    if (!backup["profiles"].is<JsonArrayConst>()) {
        return ProfileRecoveryStatus::NotFound;
    }
    for (JsonObjectConst entry : backup["profiles"].as<JsonArrayConst>()) {
        if (!entry["name"].is<const char*>()) {
            return ProfileRecoveryStatus::Invalid;
        }
        String entryName;
        const ExactV1JsonStringStatus nameStatus =
            exactV1JsonStringChecked(entry["name"], entryName, MAX_PROFILE_NAME_LEN);
        if (nameStatus == ExactV1JsonStringStatus::Unavailable) return ProfileRecoveryStatus::Unavailable;
        if (nameStatus != ExactV1JsonStringStatus::Valid) return ProfileRecoveryStatus::Invalid;
        if (entryName != canonicalName) {
            continue;
        }
        V1Profile profile;
        profile.name = std::move(entryName);
        if (!V1SettingsJson::parseRawBytes(entry["bytes"], profile.settings.bytes)) {
            Serial.printf("[Settings] Backup profile corrupt name='%s'\n", canonicalName.c_str());
            return ProfileRecoveryStatus::Invalid;
        }
        const bool hasSchema = !entry["schemaVersion"].isUnbound();
        const bool hasDetector = !entry["detector"].isUnbound();
        if (!entry["description"].isUnbound()) {
            const ExactV1JsonStringStatus descriptionStatus = exactV1JsonStringChecked(
                entry["description"], profile.description, V1_PROFILE_DESCRIPTION_MAX_BYTES);
            if (descriptionStatus == ExactV1JsonStringStatus::Unavailable) {
                return ProfileRecoveryStatus::Unavailable;
            }
            if (descriptionStatus != ExactV1JsonStringStatus::Valid) return ProfileRecoveryStatus::Invalid;
        } else if (hasSchema) {
            return ProfileRecoveryStatus::Invalid;
        }
        if (hasSchema != hasDetector ||
            (hasSchema && (!entry["schemaVersion"].is<int>() ||
                           entry["schemaVersion"].as<int>() != V1_PROFILE_SCHEMA_VERSION ||
                           !entry["detector"].is<JsonObjectConst>() ||
                           !parseV1DetectorConfiguration(entry["detector"].as<JsonObjectConst>(),
                                                         profile.detector)))) {
            Serial.printf("[Settings] Backup profile schema invalid name='%s'\n", canonicalName.c_str());
            return ProfileRecoveryStatus::Invalid;
        }
        if (hasSchema) {
            profile.schemaVersion = V1_PROFILE_SCHEMA_VERSION;
        } else {
            profile.schemaVersion = 1;
            bool displayOn = true;
            if (!entry["displayOn"].isUnbound()) {
                if (!parseBoolVariant(entry["displayOn"], displayOn)) return ProfileRecoveryStatus::Invalid;
                profile.displayOn = displayOn;
            }
            const auto readVolume = [&](const char* key, uint8_t& target) {
                if (entry[key].isUnbound()) return true;
                if (!entry[key].is<int>()) return false;
                const int value = entry[key].as<int>();
                if (!((value >= 0 && value <= 9) || value == 0xFF)) return false;
                target = static_cast<uint8_t>(value);
                return true;
            };
            if (!readVolume("mainVolume", profile.mainVolume) ||
                !readVolume("mutedVolume", profile.mutedVolume) ||
                ((profile.mainVolume == 0xFF) != (profile.mutedVolume == 0xFF))) {
                return ProfileRecoveryStatus::Invalid;
            }
        }
        const ProfileSaveResult saved = profiles.saveProfile(profile);
        if (saved.success) {
            Serial.printf("[Settings] Recovered configured profile name='%s' from validated SD backup\n",
                          canonicalName.c_str());
            return ProfileRecoveryStatus::Restored;
        }
        Serial.printf("[Settings] Failed to persist recovered profile name='%s' status=%d\n", canonicalName.c_str(),
                      static_cast<int>(saved.status));
        return ProfileRecoveryStatus::Unavailable;
    }
    return ProfileRecoveryStatus::NotFound;
}

} // namespace

bool shouldSkipProfileReferenceValidation(size_t availableProfileCount, bool hasConfiguredSlotReferences) {
    return availableProfileCount == 0 && hasConfiguredSlotReferences;
}

// --- Member methods: SD restore and validation ---

void SettingsManager::recoverCriticalSettingsAfterFullRestoreFailure(fs::FS* fs, bool hasSdBackup,
                                                                     const JsonDocument& backupDoc) {
    if (!storage_->isReady() || !storage_->isSDCard()) return;

    if (hasSdBackup && !backupDocumentCanApply(backupDoc, settings_, *profiles_)) {
        Serial.println("[Settings] Rejecting partial recovery from transactionally invalid backup");
        return;
    }

    if (hasSdBackup) {
        Serial.println("[Settings] Attempting partial recovery from SD backup");
        if (!applyBackupCriticalFieldsAtomically(
                backupDoc, settings_, *storage_,
                [](void* ctx) { return static_cast<SettingsManager*>(ctx)->save(); }, this)) {
            Serial.println("[Settings] Partial recovery failed; leaving settings and credentials unchanged");
            return;
        }
        Serial.println("[Settings] Partial recovery from SD backup applied");
    }

    if (settings_.wifiClientSSID.length() == 0) {
        const WifiClientSecretPresence secret = readWifiClientSecretPresence(fs);
        if (secret.valid() && secret.ssid.length() > 0) {
            PsramJson::Document secretDoc;
            secretDoc["wifiClientSSID"] = secret.ssid;
            if (secretDoc.overflowed()) {
                Serial.println("[Settings] WiFi secret staging unavailable; preserving current state");
                return;
            }
            bool recoveredFromSecret = false;
            if (!applyBackupWifiClientHealingAtomically(
                    secretDoc, settings_, *storage_, recoveredFromSecret,
                    [](void* ctx) { return static_cast<SettingsManager*>(ctx)->save(); }, this) ||
                !recoveredFromSecret) {
                Serial.println("[Settings] WiFi secret healing failed; preserving current state");
                return;
            }
            Serial.println("[Settings] HEAL: recovered WiFi SSID from wifi_secret");
        }
    }
}

void SettingsManager::healWifiClientSettings(fs::FS* fs, bool hasSdBackup, const JsonDocument& backupDoc) {
    const WifiClientKeyPresence keyPresence = readWifiClientKeyPresence(getActiveNamespace().c_str());
    const bool legacySsidKeyRequired = settings_.wifiClientEnabled || settings_.hasConfiguredWifiStaSlot();
    const bool keysMissing = !keyPresence.enabledKeyPresent || (legacySsidKeyRequired && !keyPresence.ssidKeyPresent);
    const bool missingCurrentSsid = settings_.wifiClientSSID.length() == 0;

    // A successful Forget may precede its SD backup. Healthy, explicitly
    // disabled NVS is authoritative even while the card still has old slots.
    if (!keysMissing && !settings_.wifiClientEnabled) return;

    if (keysMissing && !missingCurrentSsid) {
        Serial.println("[Settings] HEAL: repairing missing WiFi client keys from in-memory SSID");
        setWifiClientEnabled(true);
        return;
    }
    if (!missingCurrentSsid) return;

    if (hasSdBackup) {
        bool recoveredFromBackup = false;
        if (!applyBackupWifiClientHealingAtomically(
                backupDoc, settings_, *storage_, recoveredFromBackup,
                [](void* ctx) { return static_cast<SettingsManager*>(ctx)->save(); }, this)) {
            Serial.println("[Settings] WiFi backup healing failed; preserving current state");
            return;
        }
        if (recoveredFromBackup) {
            Serial.println("[Settings] HEAL: recovered WiFi client config from settings backup");
            return;
        }
    }

    const WifiClientSecretPresence secret = readWifiClientSecretPresence(fs);
    if (secret.status == WifiClientSecretReadStatus::Unavailable) {
        Serial.println("[Settings] WiFi secret unavailable; preserving current WiFi state");
        return;
    }
    const bool secretHasSsid = secret.valid() && secret.ssid.length() > 0;
    if (secretHasSsid) {
        PsramJson::Document secretDoc;
        secretDoc["wifiClientSSID"] = secret.ssid;
        if (secretDoc.overflowed()) {
            Serial.println("[Settings] WiFi secret staging unavailable; preserving current state");
            return;
        }
        bool recoveredFromSecret = false;
        if (!applyBackupWifiClientHealingAtomically(
                secretDoc, settings_, *storage_, recoveredFromSecret,
                [](void* ctx) { return static_cast<SettingsManager*>(ctx)->save(); }, this) ||
            !recoveredFromSecret) {
            Serial.println("[Settings] WiFi secret healing failed; preserving current state");
            return;
        }
        Serial.printf("[Settings] HEAL: recovered WiFi client config from wifi_secret (keysMissing=%s)\n",
                      keysMissing ? "yes" : "no");
    } else if (settings_.wifiClientEnabled) {
        Serial.println("[Settings] HEAL: wifiClientEnabled=true but no SSID anywhere — disabling");
        setWifiClientEnabled(false);
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
    resolveWifiCredentialTransaction();
    if (!resolveRestoreTransaction() || !resolveProfileDeleteTransaction()) {
        Serial.println("[Settings] Storage transaction recovery remains pending");
        return false;
    }
    bool needsRestore = checkNeedsRestore();
    fs::FS* fs = nullptr;
    bool hasSdBackup = false;
    PsramJson::Document bestBackupDoc;
    PsramJson::Document primaryBackupDoc;
    PsramJson::Document previousBackupDoc;
    bool hasPrimaryBackup = false;
    bool hasPreviousBackup = false;
    BackupDocumentLoadStatus bestBackupStatus = BackupDocumentLoadStatus::NotFound;
    BackupDocumentLoadStatus primaryBackupStatus = BackupDocumentLoadStatus::NotFound;
    BackupDocumentLoadStatus previousBackupStatus = BackupDocumentLoadStatus::NotFound;
    const char* bestBackupPath = nullptr;
    if (storage_->isReady() && storage_->isSDCard()) {
        fs = storage_->getFilesystem();
        StorageManager::SDLockTimed lock(storage_->getSDMutex(), 500);
        if (lock) {
            hasSdBackup = loadBestBackupDocument(fs, bestBackupDoc, &bestBackupPath, false, &bestBackupStatus);
            hasPrimaryBackup =
                parseBackupFile(fs, SETTINGS_BACKUP_PATH, primaryBackupDoc, false, &primaryBackupStatus);
            hasPreviousBackup =
                parseBackupFile(fs, SETTINGS_BACKUP_PREV_PATH, previousBackupDoc, false, &previousBackupStatus);
        } else {
            Serial.println("[Settings] SD busy while reading recovery backups; preserving profile references");
        }
    }

    if (bestBackupStatus == BackupDocumentLoadStatus::MemoryUnavailable ||
        bestBackupStatus == BackupDocumentLoadStatus::IoError ||
        primaryBackupStatus == BackupDocumentLoadStatus::MemoryUnavailable ||
        primaryBackupStatus == BackupDocumentLoadStatus::IoError ||
        previousBackupStatus == BackupDocumentLoadStatus::MemoryUnavailable ||
        previousBackupStatus == BackupDocumentLoadStatus::IoError) {
        Serial.println("[Settings] Backup recovery source unavailable; preserving NVS and SD state");
        return false;
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

    bool configuredProfilesResolved = true;
    if (!needsRestore && storage_->isReady() && storage_->isSDCard()) {
        AutoPushSlot* slots[] = {&settings_.slot0_default, &settings_.slot1_highway, &settings_.slot2_comfort};
        for (AutoPushSlot* slot : slots) {
            if (!slot || slot->profileName.length() == 0) continue;
            String canonical;
            if (canonicalizeProfileName(slot->profileName, canonical) != ProfileNameStatus::Valid) {
                configuredProfilesResolved = false;
                continue;
            }
            slot->profileName = std::move(canonical);
            V1Profile existing;
            const ProfileOperationResult loaded = profiles_->loadProfileResult(slot->profileName, existing, 0);
            if (loaded.status == ProfileStorageStatus::Success) continue;
            if (loaded.status != ProfileStorageStatus::NotFound) {
                configuredProfilesResolved = false;
                continue;
            }
            ProfileRecoveryStatus recovery = ProfileRecoveryStatus::NotFound;
            if (hasPrimaryBackup) {
                recovery = restoreProfileEntryFromBackup(primaryBackupDoc, slot->profileName, *profiles_);
            }
            if ((recovery == ProfileRecoveryStatus::NotFound || recovery == ProfileRecoveryStatus::Invalid) &&
                hasPreviousBackup) {
                recovery = restoreProfileEntryFromBackup(previousBackupDoc, slot->profileName, *profiles_);
            }
            const bool restored = recovery == ProfileRecoveryStatus::Restored;
            if (!restored) configuredProfilesResolved = false;
        }
    }

    if (!needsRestore && storage_->isReady() && storage_->isSDCard()) {
        healWifiClientSettings(fs, hasSdBackup, bestBackupDoc);
    }
    if (!needsRestore && configuredProfilesResolved && storage_->isReady() && storage_->isSDCard()) {
        synchronizeSdBackup(hasSdBackup, bestBackupPath, bestBackupDoc);
    } else if (!needsRestore && !configuredProfilesResolved) {
        Serial.println("[Settings] Preserving SD backup until configured profile recovery can complete");
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
    PsramJson::Document doc;
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

    // The validated document is now memory-resident. Release the SD transaction
    // before profile restore, whose storage boundary acquires the same mutex.
    sdLock.release();
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
    const ProfileListResult catalog = profileMgr.listProfilesResult(0);
    if (!catalog.success()) {
        Serial.printf("[Settings] Profile catalog unavailable status=%d; preserving slot profile references\n",
                      static_cast<int>(catalog.status));
        return;
    }
    if (shouldSkipProfileReferenceValidation(catalog.profiles.size(), hasConfiguredSlotReferences)) {
        Serial.println("[Settings] Profile catalog genuinely empty; preserving configured references for recovery");
        return;
    }

    // Validate that profile names in auto-push slots actually exist
    // If not, clear them to prevent repeated "file not found" errors
    bool needsSave = false;

    auto validateSlot = [&](AutoPushSlot& slot, const char* slotName) {
        if (slot.profileName.length() > 0) {
            V1Profile testProfile;
            const ProfileOperationResult loaded = profileMgr.loadProfileResult(slot.profileName, testProfile, 0);
            if (loaded.status == ProfileStorageStatus::NotFound) {
                Serial.printf("[Settings] WARN: Profile reference for %s does not exist - clearing reference\n",
                              slotName);
                slot.profileName = "";
                needsSave = true;
            } else if (!loaded.success()) {
                Serial.printf("[Settings] Profile reference for %s could not be validated status=%d; preserving\n",
                              slotName, static_cast<int>(loaded.status));
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
