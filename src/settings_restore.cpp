/**
 * Settings SD restore and validation paths.
 * Extracted from settings_backup.cpp to keep backup writer focused.
 */

#include "settings_internals.h"
#include <nvs.h>
#include "settings_backup_doc.h"

bool shouldSkipProfileReferenceValidation(size_t availableProfileCount, bool hasConfiguredSlotReferences) {
    return availableProfileCount == 0 && hasConfiguredSlotReferences;
}

// --- Member methods: SD restore and validation ---

bool SettingsManager::checkAndRestoreFromSD() {
    // Check if NVS was erased (appears default) and backup exists on SD
    // This can be called after storage is mounted to retry the restore
    bool needsRestore = checkNeedsRestore();
    fs::FS* fs = nullptr;
    bool hasSdBackup = false;
    JsonDocument bestBackupDoc;
    const char* bestBackupPath = nullptr;
    if (storageManager.isReady() && storageManager.isSDCard()) {
        fs = storageManager.getFilesystem();
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

        // Full restore failed (no backup file, or SD not mounted yet).
        // Attempt partial recovery from whatever sources are available so the
        // device boots with WiFi, profiles, and other critical settings intact
        // instead of falling back to factory defaults permanently.
        if (storageManager.isReady() && storageManager.isSDCard()) {
            bool partialRecovered = false;

            // Recover as many settings as possible from the best backup document.
            // This covers the case where loadBestBackupDocument succeeded above
            // but restoreFromSD failed for a different reason (e.g. mutex).
            if (hasSdBackup) {
                Serial.println("[Settings] Attempting partial recovery from SD backup");
                // Recover WiFi settings
                bool backupEnableWifi = true;
                if (parseBoolVariant(bestBackupDoc["enableWifi"], backupEnableWifi)) {
                    settings_.enableWifi = backupEnableWifi;
                }
                const char* backupApSSID = bestBackupDoc["apSSID"] | "";
                if (backupApSSID[0] != '\0') {
                    settings_.apSSID = sanitizeApSsidValue(String(backupApSSID));
                }
                bool backupWifiClientEnabled = false;
                parseBoolVariant(bestBackupDoc["wifiClientEnabled"], backupWifiClientEnabled);
                const String backupSsid = legacyWifiClientSsidFromBackupDoc(bestBackupDoc);
                if (restoreWifiStaSlotsFromBackupDoc(bestBackupDoc, settings_, false)) {
                    settings_.wifiClientEnabled = true;
                    settings_.wifiMode = V1_WIFI_APSTA;
                } else if (backupSsid.length() > 0) {
                    settings_.wifiClientEnabled = true;
                    settings_.wifiClientSSID = backupSsid;
                    settings_.ensureWifiStaSlotForLegacyAlias();
                    settings_.wifiMode = V1_WIFI_APSTA;
                } else if (backupWifiClientEnabled) {
                    settings_.wifiClientEnabled = backupWifiClientEnabled;
                    settings_.refreshWifiClientAliasFromSlots();
                }
                // Recover profile slot bindings
                if (bestBackupDoc["slot0ProfileName"].is<const char*>()) {
                    settings_.slot0_default.profileName = bestBackupDoc["slot0ProfileName"].as<String>();
                }
                if (bestBackupDoc["slot1ProfileName"].is<const char*>()) {
                    settings_.slot1_highway.profileName = bestBackupDoc["slot1ProfileName"].as<String>();
                }
                if (bestBackupDoc["slot2ProfileName"].is<const char*>()) {
                    settings_.slot2_comfort.profileName = bestBackupDoc["slot2ProfileName"].as<String>();
                }
                if (bestBackupDoc["slot0Mode"].is<int>()) {
                    settings_.slot0_default.mode = normalizeV1ModeValue(bestBackupDoc["slot0Mode"].as<int>());
                }
                if (bestBackupDoc["slot1Mode"].is<int>()) {
                    settings_.slot1_highway.mode = normalizeV1ModeValue(bestBackupDoc["slot1Mode"].as<int>());
                }
                if (bestBackupDoc["slot2Mode"].is<int>()) {
                    settings_.slot2_comfort.mode = normalizeV1ModeValue(bestBackupDoc["slot2Mode"].as<int>());
                }
                bool backupAutoPush = false;
                if (parseBoolVariant(bestBackupDoc["autoPushEnabled"], backupAutoPush)) {
                    settings_.autoPushEnabled = backupAutoPush;
                }
                if (bestBackupDoc["activeSlot"].is<int>()) {
                    settings_.activeSlot = bestBackupDoc["activeSlot"].as<int>();
                }
                if (bestBackupDoc["brightness"].is<int>()) {
                    settings_.brightness = clampU8(bestBackupDoc["brightness"].as<int>(), 1, 255);
                }
                // Recover apPassword (obfuscated — same decode path as applyBackupDocument)
                if (bestBackupDoc["apPassword"].is<const char*>()) {
                    String decoded = decodeObfuscatedFromStorage(bestBackupDoc["apPassword"].as<String>());
                    if (decoded.length() >= MIN_AP_PASSWORD_LEN) {
                        settings_.apPassword = sanitizeApPasswordValue(decoded);
                    }
                }
                restoreWifiClientPasswordObfFromBackupDoc(bestBackupDoc, settings_.wifiClientSSID);
                restoreLegacyStationPasswordFromBackupDoc(bestBackupDoc, settings_.wifiClientSSID);
                // Recover UI hide-flags
                bool boolVal = false;
                if (parseBoolVariant(bestBackupDoc["hideWifiIcon"], boolVal))
                    settings_.hideWifiIcon = boolVal;
                if (parseBoolVariant(bestBackupDoc["hideProfileIndicator"], boolVal))
                    settings_.hideProfileIndicator = boolVal;
                if (parseBoolVariant(bestBackupDoc["hideBatteryIcon"], boolVal))
                    settings_.hideBatteryIcon = boolVal;
                if (parseBoolVariant(bestBackupDoc["showBatteryPercent"], boolVal))
                    settings_.showBatteryPercent = boolVal;
                if (parseBoolVariant(bestBackupDoc["hideBleIcon"], boolVal))
                    settings_.hideBleIcon = boolVal;
                if (parseBoolVariant(bestBackupDoc["hideVolumeIndicator"], boolVal))
                    settings_.hideVolumeIndicator = boolVal;
                if (parseBoolVariant(bestBackupDoc["hideRssiIndicator"], boolVal))
                    settings_.hideRssiIndicator = boolVal;
                // Recover colors
                if (bestBackupDoc["colorBogey"].is<int>())
                    settings_.colorBogey = sanitizeRgb565Color(bestBackupDoc["colorBogey"], 0xF800);
                if (bestBackupDoc["colorFrequency"].is<int>())
                    settings_.colorFrequency = sanitizeRgb565Color(bestBackupDoc["colorFrequency"], 0xF800);
                if (bestBackupDoc["colorArrowFront"].is<int>())
                    settings_.colorArrowFront = sanitizeRgb565Color(bestBackupDoc["colorArrowFront"], 0xF800);
                if (bestBackupDoc["colorArrowSide"].is<int>())
                    settings_.colorArrowSide = sanitizeRgb565Color(bestBackupDoc["colorArrowSide"], 0xF800);
                if (bestBackupDoc["colorArrowRear"].is<int>())
                    settings_.colorArrowRear = sanitizeRgb565Color(bestBackupDoc["colorArrowRear"], 0xF800);
                if (bestBackupDoc["colorBandL"].is<int>())
                    settings_.colorBandL = sanitizeRgb565Color(bestBackupDoc["colorBandL"], 0x001F);
                if (bestBackupDoc["colorBandKa"].is<int>())
                    settings_.colorBandKa = sanitizeRgb565Color(bestBackupDoc["colorBandKa"], 0xF800);
                if (bestBackupDoc["colorBandK"].is<int>())
                    settings_.colorBandK = sanitizeRgb565Color(bestBackupDoc["colorBandK"], 0x001F);
                if (bestBackupDoc["colorBandX"].is<int>())
                    settings_.colorBandX = sanitizeRgb565Color(bestBackupDoc["colorBandX"], 0x07E0);
                if (bestBackupDoc["colorBandPhoto"].is<int>())
                    settings_.colorBandPhoto = sanitizeRgb565Color(bestBackupDoc["colorBandPhoto"], 0x780F);
                if (bestBackupDoc["colorAlpConnected"].is<int>())
                    settings_.colorAlpConnected = sanitizeRgb565Color(bestBackupDoc["colorAlpConnected"], 0x07E0);
                if (bestBackupDoc["colorAlpDli"].is<int>())
                    settings_.colorAlpDli = sanitizeRgb565Color(bestBackupDoc["colorAlpDli"], 0xFD20);
                if (bestBackupDoc["colorAlpLidActive"].is<int>())
                    settings_.colorAlpLidActive = sanitizeRgb565Color(bestBackupDoc["colorAlpLidActive"], 0x001F);
                if (bestBackupDoc["colorAlpAlert"].is<int>())
                    settings_.colorAlpAlert = sanitizeRgb565Color(bestBackupDoc["colorAlpAlert"], 0xF800);
                if (bestBackupDoc["colorObd"].is<int>())
                    settings_.colorObd = sanitizeRgb565Color(bestBackupDoc["colorObd"], 0x001F);
                // Recover speed-mute settings
                if (parseBoolVariant(bestBackupDoc["speedMuteEnabled"], boolVal))
                    settings_.speedMuteEnabled = boolVal;
                if (bestBackupDoc["speedMuteThresholdMph"].is<int>()) {
                    settings_.speedMuteThresholdMph = clampU8(bestBackupDoc["speedMuteThresholdMph"].as<int>(), 5, 60);
                }
                if (bestBackupDoc["speedMuteHysteresisMph"].is<int>()) {
                    settings_.speedMuteHysteresisMph =
                        clampU8(bestBackupDoc["speedMuteHysteresisMph"].as<int>(), 1, 10);
                }
                if (bestBackupDoc["speedMuteVolume"].is<int>()) {
                    const int raw = bestBackupDoc["speedMuteVolume"].as<int>();
                    settings_.speedMuteVolume = (raw >= 0 && raw <= 9) ? static_cast<uint8_t>(raw) : 0;
                }
                if (parseBoolVariant(bestBackupDoc["stealthEnabled"], boolVal))
                    settings_.stealthEnabled = boolVal;
                // Recover OBD settings
                if (parseBoolVariant(bestBackupDoc["obdEnabled"], boolVal))
                    settings_.obdEnabled = boolVal;
                if (bestBackupDoc["obdSavedName"].is<const char*>()) {
                    settings_.obdSavedName = sanitizeObdSavedNameValue(bestBackupDoc["obdSavedName"].as<String>());
                }
                // Recover ALP settings
                if (parseBoolVariant(bestBackupDoc["alpEnabled"], boolVal))
                    settings_.alpEnabled = boolVal;
                if (parseBoolVariant(bestBackupDoc["alpSdLogEnabled"], boolVal))
                    settings_.alpSdLogEnabled = boolVal;
                if (bestBackupDoc["alpAlertPersistSec"].is<int>()) {
                    settings_.alpAlertPersistSec = clampU8(bestBackupDoc["alpAlertPersistSec"].as<int>(), 0, 5);
                }
                if (parseBoolVariant(bestBackupDoc["alpDisableV1LaserOnPush"], boolVal)) {
                    settings_.alpDisableV1LaserOnPush = boolVal;
                }
                // Recover GPS settings
                if (parseBoolVariant(bestBackupDoc["gpsEnabled"], boolVal))
                    settings_.gpsEnabled = boolVal;
                if (bestBackupDoc["gpsBaud"].is<int>()) {
                    settings_.gpsBaud = sanitizeGpsBaudValue(static_cast<uint32_t>(bestBackupDoc["gpsBaud"].as<int>()));
                }
                settings_.gpsEnablePinActiveHigh = true;
                if (parseBoolVariant(bestBackupDoc["gpsLogUtcToPerf"], boolVal))
                    settings_.gpsLogUtcToPerf = boolVal;
                if (parseBoolVariant(bestBackupDoc["gpsLogUtcToAlp"], boolVal))
                    settings_.gpsLogUtcToAlp = boolVal;
                if (settings_.proxyBLE && settings_.obdEnabled) {
                    Serial.println("[Settings] HEAL: recovered proxyBLE+obdEnabled — keeping OBD, disabling proxy");
                    settings_.proxyBLE = false;
                }
                Serial.println("[Settings] Partial recovery from SD backup applied");
                partialRecovered = true;
            }

            // WiFi secret file fallback (covers case where no backup exists at all)
            if (settings_.wifiClientSSID.length() == 0) {
                const WifiClientSecretPresence secretPresence = readWifiClientSecretPresence(fs);
                if (secretPresence.valid && secretPresence.ssid.length() > 0) {
                    settings_.wifiClientEnabled = true;
                    settings_.wifiClientSSID = secretPresence.ssid;
                    settings_.ensureWifiStaSlotForLegacyAlias();
                    settings_.wifiMode = V1_WIFI_APSTA;
                    Serial.printf("[Settings] HEAL: recovered WiFi SSID from wifi_secret ('%s')\n",
                                  settings_.wifiClientSSID.c_str());
                    partialRecovered = true;
                }
            }

            if (partialRecovered) {
                save();
                backupToSD();
            }
        }
    } else if (hasSdBackup) {
        // Keep user/NVS state authoritative unless corruption is detected.
        // Slot/profile healing is handled separately by validateProfileReferences().
        Serial.println("[Settings] NVS healthy; skipping automatic SD settings_ restore");
    }

    if (!needsRestore && storageManager.isReady() && storageManager.isSDCard()) {
        const WifiClientKeyPresence wifiKeyPresence = readWifiClientKeyPresence(getActiveNamespace().c_str());
        const bool wifiKeysMissing = !wifiKeyPresence.enabledKeyPresent || !wifiKeyPresence.ssidKeyPresent;
        const bool missingCurrentSsid = settings_.wifiClientSSID.length() == 0;

        if (wifiKeysMissing && !missingCurrentSsid) {
            // SSID is already present in memory; rewrite namespace to restore missing keys.
            settings_.wifiClientEnabled = true;
            settings_.wifiMode = V1_WIFI_APSTA;
            Serial.println("[Settings] HEAL: repairing missing WiFi client keys from in-memory SSID");
            save();
        } else if (missingCurrentSsid) {
            bool backupWifiClientEnabled = false;
            const bool backupEnabledKnown =
                hasSdBackup && parseBoolVariant(bestBackupDoc["wifiClientEnabled"], backupWifiClientEnabled);
            const String backupSsid = hasSdBackup ? legacyWifiClientSsidFromBackupDoc(bestBackupDoc) : "";
            const bool backupHasSsid = backupSsid.length() > 0;
            const bool backupHasStaSlots = hasSdBackup && bestBackupDoc["wifiStaSlots"].is<JsonArrayConst>();

            const WifiClientSecretPresence secretPresence = readWifiClientSecretPresence(fs);
            const bool secretHasSsid = secretPresence.valid && secretPresence.ssid.length() > 0;

            String recoveredSsid = "";
            const char* recoveredFrom = "none";
            bool recoveredFromSlots = false;
            if (backupHasStaSlots && restoreWifiStaSlotsFromBackupDoc(bestBackupDoc, settings_, false)) {
                recoveredSsid = settings_.wifiClientSSID;
                recoveredFrom = "settings_backup_slots";
                recoveredFromSlots = true;
            } else if (backupHasSsid) {
                recoveredSsid = backupSsid;
                recoveredFrom = "settings_backup";
            } else if (secretHasSsid) {
                recoveredSsid = secretPresence.ssid;
                recoveredFrom = "wifi_secret";
            }

            // Targeted WiFi credential recovery:
            // - legacy case: wifiClientEnabled=true but SSID missing
            // - partial-key case: WiFi client keys missing from NVS
            // - backup-missing case: recover SSID from SD WiFi secret metadata
            const bool shouldRecoverWifiClient =
                recoveredSsid.length() > 0 && (settings_.wifiClientEnabled || wifiKeysMissing ||
                                               (backupEnabledKnown && backupWifiClientEnabled) || secretHasSsid);

            if (shouldRecoverWifiClient) {
                settings_.wifiClientEnabled = true;
                if (!recoveredFromSlots) {
                    settings_.wifiClientSSID = recoveredSsid;
                    settings_.ensureWifiStaSlotForLegacyAlias();
                }
                settings_.wifiMode = V1_WIFI_APSTA;
                Serial.printf("[Settings] HEAL: recovered WiFi client config from %s (ssid='%s', keysMissing=%s)\n",
                              recoveredFrom, settings_.wifiClientSSID.c_str(), wifiKeysMissing ? "yes" : "no");
                if (backupHasSsid) {
                    restoreWifiClientPasswordObfFromBackupDoc(bestBackupDoc, settings_.wifiClientSSID);
                    restoreLegacyStationPasswordFromBackupDoc(bestBackupDoc, settings_.wifiClientSSID);
                }
                save();
            } else if (settings_.wifiClientEnabled) {
                // SSID missing in all recovery sources — disable to avoid inconsistent state.
                settings_.wifiClientEnabled = false;
                settings_.wifiMode = V1_WIFI_AP;
                Serial.println("[Settings] HEAL: wifiClientEnabled=true but no SSID anywhere — disabling");
                save();
            } else if (wifiKeysMissing) {
                Serial.println("[Settings] WARN: WiFi client keys missing and no SSID recovery source found");
            }
        }
    }

    // Keep SD backup schema fresh so newly added settings survive the next reflash.
    if (!needsRestore && storageManager.isReady() && storageManager.isSDCard()) {
        if (!hasSdBackup) {
            Serial.println("[Settings] No valid SD backup found; creating backup from current settings_");
            backupToSD();
        } else {
            const int backupVersion = backupDocumentVersion(bestBackupDoc);
            const bool missingCoreFields = bestBackupDoc["brightness"].isNull();
            const bool backupOutOfSync = !backupAppearsInSyncWithNvs(bestBackupDoc, settings_);
            if (backupVersion < SD_BACKUP_VERSION || missingCoreFields || backupOutOfSync) {
                Serial.printf("[Settings] Refreshing SD backup schema (path=%s version=%d)\n",
                              bestBackupPath ? bestBackupPath : "(unknown)", backupVersion);
                if (backupOutOfSync) {
                    Serial.println("[Settings] SD backup differs from healthy NVS; refreshing backup content");
                }
                backupToSD();
            }
        }
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
    if (!storageManager.isReady() || !storageManager.isSDCard()) {
        return false;
    }

    // Acquire SD mutex to protect file I/O
    StorageManager::SDLockBlocking sdLock(storageManager.getSDMutex());
    if (!sdLock) {
        Serial.println("[Settings] Failed to acquire SD mutex for restore");
        return false;
    }

    fs::FS* fs = storageManager.getFilesystem();
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
    const char* backupSlot0 =
        doc["slot0ProfileName"].is<const char*>() ? doc["slot0ProfileName"].as<const char*>() : "";
    const int backupSlot0Mode = doc["slot0Mode"].is<int>() ? doc["slot0Mode"].as<int>() : -1;
    Serial.printf("[Settings] Backup fields: autoPush=%s slot0Profile='%s' slot0Mode=%d\n",
                  hasAutoPush ? (backupAutoPush ? "true" : "false") : "missing", backupSlot0, backupSlot0Mode);

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
                Serial.printf("[Settings] WARN: Profile '%s' for %s does not exist - clearing reference\n",
                              slot.profileName.c_str(), slotName);
                slot.profileName = "";
                needsSave = true;
            } else {
                Serial.printf("[Settings] Profile '%s' for %s validated OK\n", slot.profileName.c_str(), slotName);
            }
        }
    };

    validateSlot(settings_.slot0_default, "Slot 0 (Default)");
    validateSlot(settings_.slot1_highway, "Slot 1 (Highway)");
    validateSlot(settings_.slot2_comfort, "Slot 2 (Comfort)");

    if (needsSave) {
        if (persistSettingsAtomically()) {
            bumpBackupRevision();
            Serial.println("[Settings] Cleared invalid profile references and saved");
        } else {
            Serial.println("[Settings] ERROR: Failed to persist cleared profile references");
        }
    }

    // No additional side effects needed beyond clearing invalid references.
}
