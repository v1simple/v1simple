/**
 * Settings backup-document helpers and the backup-apply path.
 * Extracted from settings_restore.cpp (pure move) so the SD restore and
 * validation member methods stay in settings_restore.cpp.
 */

#include "settings_backup_doc.h"

bool loadBestBackupDocument(fs::FS* fs, JsonDocument& outDoc, const char** outPath, bool verboseErrors) {
    if (!fs) {
        return false;
    }

    int bestScore = -1;
    const char* bestPath = nullptr;
    String bestJson;
    JsonDocument candidateDoc;

    for (size_t i = 0; i < SETTINGS_BACKUP_CANDIDATES_COUNT; ++i) {
        const char* candidate = SETTINGS_BACKUP_CANDIDATES[i];
        if (!fs->exists(candidate)) {
            continue;
        }

        candidateDoc.clear();
        if (!parseBackupFile(fs, candidate, candidateDoc, verboseErrors)) {
            if (verboseErrors) {
                Serial.printf("[Settings] WARN: Ignoring invalid backup candidate: %s\n", candidate);
            }
            continue;
        }

        const int score = backupCandidateScore(candidateDoc);
        if (score > bestScore) {
            bestScore = score;
            bestPath = candidate;
            bestJson = "";
            serializeJson(candidateDoc, bestJson);
        }
    }

    if (bestScore < 0 || bestJson.length() == 0 || !bestPath) {
        return false;
    }

    outDoc.clear();
    DeserializationError err = deserializeJson(outDoc, bestJson);
    if (err) {
        if (verboseErrors) {
            Serial.printf("[Settings] Failed to parse selected backup '%s': %s\n", bestPath, err.c_str());
        }
        return false;
    }

    if (outPath) {
        *outPath = bestPath;
    }
    return true;
}

bool parseBoolVariant(const JsonVariantConst& value, bool& out) {
    if (value.isNull()) {
        return false;
    }
    if (value.is<bool>()) {
        out = value.as<bool>();
        return true;
    }
    if (value.is<int>()) {
        out = value.as<int>() != 0;
        return true;
    }
    if (value.is<const char*>()) {
        String raw = value.as<String>();
        raw.trim();
        raw.toLowerCase();
        if (raw == "1" || raw == "true" || raw == "on" || raw == "yes") {
            out = true;
            return true;
        }
        if (raw == "0" || raw == "false" || raw == "off" || raw == "no") {
            out = false;
            return true;
        }
    }
    return false;
}

bool restoreWifiClientPasswordObfFromBackupDoc(const JsonDocument& doc, const String& expectedSsid) {
    static constexpr const char* WIFI_CLIENT_BACKUP_PASSWORD_KEY = "wifiClientPasswordObf";
    if (!doc[WIFI_CLIENT_BACKUP_PASSWORD_KEY].is<const char*>()) {
        return false;
    }
    if (expectedSsid.length() == 0) {
        return false;
    }

    const String backupSsid = doc["wifiClientSSID"] | "";
    if (backupSsid.length() > 0 && backupSsid != expectedSsid) {
        Serial.printf("[Settings] WARN: Skipping WiFi client password restore; SSID mismatch (want='%s' got='%s')\n",
                      expectedSsid.c_str(), backupSsid.c_str());
        return false;
    }

    const String encoded = doc[WIFI_CLIENT_BACKUP_PASSWORD_KEY].as<String>();
    if (encoded.length() == 0 || decodeObfuscatedFromStorage(encoded).length() == 0) {
        Serial.println("[Settings] WARN: Skipping corrupt WiFi client password in backup");
        return false;
    }

    if (!storeWifiClientPasswordObfToNvs(encoded)) {
        Serial.println("[Settings] WARN: Failed to restore WiFi client password to NVS");
        return false;
    }

    Serial.println("[Settings] Restored WiFi client password from settings backup");
    return true;
}

String legacyWifiClientSsidFromBackupDoc(const JsonDocument& doc) {
    if (doc["wifiClientSSID"].is<const char*>()) {
        String ssid = sanitizeWifiClientSsidValue(doc["wifiClientSSID"].as<String>());
        if (ssid.length() > 0) {
            return ssid;
        }
    }
    if (doc["stationSSID"].is<const char*>()) {
        return sanitizeWifiClientSsidValue(doc["stationSSID"].as<String>());
    }
    return "";
}

bool restoreLegacyStationPasswordFromBackupDoc(const JsonDocument& doc, const String& expectedSsid) {
    static constexpr const char* LEGACY_STATION_PASSWORD_KEY = "stationPassword";
    if (!doc[LEGACY_STATION_PASSWORD_KEY].is<const char*>()) {
        return false;
    }
    if (expectedSsid.length() == 0) {
        return false;
    }

    const String backupSsid = legacyWifiClientSsidFromBackupDoc(doc);
    if (backupSsid.length() > 0 && backupSsid != expectedSsid) {
        Serial.printf("[Settings] WARN: Skipping legacy station password restore; SSID mismatch (want='%s' got='%s')\n",
                      expectedSsid.c_str(), backupSsid.c_str());
        return false;
    }

    const String sanitizedPassword = sanitizeWifiClientPasswordValue(doc[LEGACY_STATION_PASSWORD_KEY].as<String>());
    if (sanitizedPassword.length() == 0) {
        return false;
    }

    if (!storeWifiClientPasswordObfToNvs(encodeObfuscatedForStorage(sanitizedPassword), 0)) {
        Serial.println("[Settings] WARN: Failed to restore legacy station password to NVS");
        return false;
    }

    Serial.println("[Settings] Restored legacy station password from settings backup");
    return true;
}

bool restoreWifiStaSlotPasswordObfFromBackupSlot(JsonObjectConst slotObj, size_t index) {
    static constexpr const char* WIFI_STA_SLOT_PASSWORD_OBF_KEY = "passwordObf";
    if (!slotObj[WIFI_STA_SLOT_PASSWORD_OBF_KEY].is<const char*>()) {
        return false;
    }

    const String encoded = slotObj[WIFI_STA_SLOT_PASSWORD_OBF_KEY].as<String>();
    if (encoded.length() == 0 || decodeObfuscatedFromStorage(encoded).length() == 0) {
        Serial.printf("[Settings] WARN: Skipping corrupt WiFi STA slot %u password in backup\n",
                      static_cast<unsigned>(index));
        return false;
    }

    if (!storeWifiClientPasswordObfToNvs(encoded, index)) {
        Serial.printf("[Settings] WARN: Failed to restore WiFi STA slot %u password to NVS\n",
                      static_cast<unsigned>(index));
        return false;
    }

    Serial.printf("[Settings] Restored WiFi STA slot %u password from settings backup\n", static_cast<unsigned>(index));
    return true;
}

// Snapshot of the password each slot held (keyed by that slot's SSID) taken
// before a restore wipes them, so passwords survive a sanitized backup (one
// exported without passwordObf) whose slots keep the same network names.
// Explicit passwordObf entries in the backup always win over the snapshot.
struct StoredSlotPasswordSnapshot {
    String ssid;
    String passwordObf;
};

void snapshotWifiStaSlotPasswords(const V1Settings& settings,
                                  StoredSlotPasswordSnapshot (&snapshot)[kWifiStaSlotCount]) {
    Preferences prefs;
    const bool opened = prefs.begin(WIFI_CLIENT_NS, true);
    for (size_t i = 0; i < kWifiStaSlotCount; ++i) {
        snapshot[i].ssid = settings.wifiStaSlots[i].ssid;
        snapshot[i].passwordObf = "";
        if (opened && snapshot[i].ssid.length() > 0 && prefs.isKey(kNvsWifiStaSlotPassword[i])) {
            snapshot[i].passwordObf = prefs.getString(kNvsWifiStaSlotPassword[i], "");
        }
    }
    if (opened) {
        prefs.end();
    }
}

bool preserveStoredPasswordForMatchingSsid(const String& ssid, size_t targetIndex,
                                           const StoredSlotPasswordSnapshot (&snapshot)[kWifiStaSlotCount]) {
    for (size_t i = 0; i < kWifiStaSlotCount; ++i) {
        if (snapshot[i].passwordObf.length() == 0 || snapshot[i].ssid != ssid) {
            continue;
        }
        if (storeWifiClientPasswordObfToNvs(snapshot[i].passwordObf, targetIndex)) {
            Serial.printf("[Settings] Preserved stored WiFi password for slot %u (SSID match)\n",
                          static_cast<unsigned>(targetIndex));
            return true;
        }
        return false;
    }
    return false;
}

void clearWifiStaSlotPasswordsForRestore(bool clearSdSecret) {
    Preferences prefs;
    if (prefs.begin(WIFI_CLIENT_NS, false)) {
        for (size_t i = 0; i < kWifiStaSlotCount; ++i) {
            prefs.remove(kNvsWifiStaSlotPassword[i]);
        }
        prefs.remove(kNvsWifiPassword);
        prefs.end();
    }
    if (clearSdSecret) {
        clearWifiClientSecretFromSD();
    }
}

bool restoreWifiStaSlotsFromBackupDoc(const JsonDocument& doc, V1Settings& settings, bool clearSdSecret) {
    if (!doc["wifiStaSlots"].is<JsonArrayConst>()) {
        return false;
    }
    bool parsedWifiClientEnabled = false;
    const bool wifiClientEnabledExplicit = parseBoolVariant(doc["wifiClientEnabled"], parsedWifiClientEnabled);

    StoredSlotPasswordSnapshot storedPasswords[kWifiStaSlotCount];
    snapshotWifiStaSlotPasswords(settings, storedPasswords);

    clearWifiStaSlotPasswordsForRestore(clearSdSecret);

    for (size_t i = 0; i < kWifiStaSlotCount; ++i) {
        settings.wifiStaSlots[i] = WifiStaSlot();
        settings.wifiStaSlots[i].priority = static_cast<uint8_t>(i);
    }

    bool restoredAny = false;
    JsonArrayConst slots = doc["wifiStaSlots"].as<JsonArrayConst>();
    for (JsonObjectConst slotObj : slots) {
        if (!slotObj["ssid"].is<const char*>()) {
            continue;
        }
        int index = slotObj["index"] | -1;
        if (index < 0 || index >= static_cast<int>(kWifiStaSlotCount)) {
            continue;
        }

        WifiStaSlot& slot = settings.wifiStaSlots[static_cast<size_t>(index)];
        slot.ssid = sanitizeWifiClientSsidValue(slotObj["ssid"].as<String>());
        if (slot.ssid.length() == 0) {
            continue;
        }
        slot.label = sanitizeWifiStaSlotLabelValue(slotObj["label"] | "");
        if (slot.label.length() == 0) {
            slot.label = (index == 0) ? "Saved" : slot.ssid;
        }
        slot.priority = slotObj["priority"].is<int>() ? clampU8(slotObj["priority"].as<int>(), 0, 255)
                                                      : static_cast<uint8_t>(index);
        if (slotObj["lastConnectedAtSec"].is<uint32_t>()) {
            slot.lastConnectedAtSec = slotObj["lastConnectedAtSec"].as<uint32_t>();
        } else if (slotObj["lastConnectedAtSec"].is<int>()) {
            slot.lastConnectedAtSec = static_cast<uint32_t>(std::max(0, slotObj["lastConnectedAtSec"].as<int>()));
        } else {
            slot.lastConnectedAtSec = 0;
        }
        if (!restoreWifiStaSlotPasswordObfFromBackupSlot(slotObj, static_cast<size_t>(index))) {
            // Sanitized backup (no passwordObf): keep the stored password when
            // the incoming slot names a network we already have credentials for.
            preserveStoredPasswordForMatchingSsid(slot.ssid, static_cast<size_t>(index), storedPasswords);
        }
        restoredAny = true;
    }

    if (restoredAny && !settings.wifiClientEnabled && !wifiClientEnabledExplicit) {
        settings.wifiClientEnabled = true;
    }
    settings.refreshWifiClientAliasFromSlots();
    return true;
}

// Profile entries processed between watchdog feeds inside the profile restore
// loop.  Every entry costs a filesystem write, so feeding per batch bounds the
// gap between feeds without putting a feed on the per-field path.
static constexpr int kProfileRestoreWatchdogFeedInterval = 4;

SettingsBackupApplyResult SettingsManager::applyBackupDocument(const JsonDocument& doc, bool deferBackupRewrite,
                                                               const SettingsRestoreWatchdog& watchdog) {
    SettingsBackupApplyResult result;

    // Fed at restore phase boundaries only — see SettingsRestoreWatchdog.
    auto feedWatchdog = [&watchdog]() {
        if (watchdog.feed) {
            watchdog.feed(watchdog.ctx);
        }
    };

    auto restoreBool = [&](const char* key, bool& target) {
        bool parsed = false;
        if (parseBoolVariant(doc[key], parsed)) {
            target = parsed;
        }
    };

    // ============================================================================
    // WiFi/Network Settings
    // ============================================================================
    // AP password: restore from backup if the key is present (clean-flash path).
    // If the backup is older and lacks the key, leave whatever load() produced —
    // that preserves the existing NVS password on in-place restores.
    if (doc["apPassword"].is<const char*>()) {
        String decoded = decodeObfuscatedFromStorage(doc["apPassword"].as<String>());
        if (decoded.length() >= MIN_AP_PASSWORD_LEN) {
            settings_.apPassword = sanitizeApPasswordValue(decoded);
        }
        // else: decoded value is too short / corrupt — keep existing
    }
    restoreBool("enableWifi", settings_.enableWifi);
    if (doc["apSSID"].is<const char*>())
        settings_.apSSID = sanitizeApSsidValue(doc["apSSID"].as<String>());
    bool parsedWifiClientEnabled = false;
    const bool wifiClientEnabledExplicit = parseBoolVariant(doc["wifiClientEnabled"], parsedWifiClientEnabled);
    if (wifiClientEnabledExplicit) {
        settings_.wifiClientEnabled = parsedWifiClientEnabled;
    }
    const bool restoredWifiStaSlots = restoreWifiStaSlotsFromBackupDoc(doc, settings_, deferBackupRewrite);
    const String legacyWifiClientSsid = legacyWifiClientSsidFromBackupDoc(doc);
    if (!restoredWifiStaSlots && legacyWifiClientSsid.length() > 0) {
        clearWifiStaSlotPasswordsForRestore(deferBackupRewrite);
        for (size_t i = 0; i < kWifiStaSlotCount; ++i) {
            settings_.wifiStaSlots[i] = WifiStaSlot();
        }
        settings_.wifiStaSlots[0].ssid = legacyWifiClientSsid;
        if (settings_.wifiStaSlots[0].ssid.length() > 0) {
            settings_.wifiStaSlots[0].label = "Saved";
        }
        settings_.wifiStaSlots[0].priority = 0;
        settings_.refreshWifiClientAliasFromSlots();
    }
    if (!settings_.wifiClientEnabled && settings_.hasConfiguredWifiStaSlot() && !wifiClientEnabledExplicit) {
        settings_.wifiClientEnabled = true;
    }
    settings_.refreshWifiClientAliasFromSlots();
    restoreWifiClientPasswordObfFromBackupDoc(doc, settings_.wifiClientSSID);
    restoreLegacyStationPasswordFromBackupDoc(doc, settings_.wifiClientSSID);

    // Phase 1 done: the WiFi credential block above is the only field-restore
    // work that touches NVS and the SD secret file.
    feedWatchdog();

    restoreBool("proxyBLE", settings_.proxyBLE);
    if (doc["proxyName"].is<const char*>()) {
        settings_.proxyName = sanitizeProxyNameValue(doc["proxyName"].as<String>());
    }
    if (doc["lastV1Address"].is<const char*>()) {
        settings_.lastV1Address = sanitizeLastV1AddressValue(doc["lastV1Address"].as<String>());
    }
    if (doc["autoPowerOffMinutes"].is<int>()) {
        settings_.autoPowerOffMinutes = clampU8(doc["autoPowerOffMinutes"].as<int>(), 0, 60);
    }
    if (doc["apTimeoutMinutes"].is<int>()) {
        settings_.apTimeoutMinutes = clampApTimeoutValue(doc["apTimeoutMinutes"].as<int>());
    }

    // ============================================================================
    // Display Settings
    // ============================================================================
    if (doc["brightness"].is<int>())
        settings_.brightness = clampU8(doc["brightness"].as<int>(), 1, 255);
    restoreBool("turnOffDisplay", settings_.turnOffDisplay);

    // ============================================================================
    // All Colors (sanitized identically to the NVS-load path in settings.cpp)
    // ============================================================================
    if (doc["colorBogey"].is<int>())
        settings_.colorBogey = sanitizeRgb565Color(doc["colorBogey"], 0xF800);
    if (doc["colorFrequency"].is<int>())
        settings_.colorFrequency = sanitizeRgb565Color(doc["colorFrequency"], 0xF800);
    if (doc["colorArrowFront"].is<int>())
        settings_.colorArrowFront = sanitizeRgb565Color(doc["colorArrowFront"], 0xF800);
    if (doc["colorArrowSide"].is<int>())
        settings_.colorArrowSide = sanitizeRgb565Color(doc["colorArrowSide"], 0xF800);
    if (doc["colorArrowRear"].is<int>())
        settings_.colorArrowRear = sanitizeRgb565Color(doc["colorArrowRear"], 0xF800);
    if (doc["colorBandL"].is<int>())
        settings_.colorBandL = sanitizeRgb565Color(doc["colorBandL"], 0x001F);
    if (doc["colorBandKa"].is<int>())
        settings_.colorBandKa = sanitizeRgb565Color(doc["colorBandKa"], 0xF800);
    if (doc["colorBandK"].is<int>())
        settings_.colorBandK = sanitizeRgb565Color(doc["colorBandK"], 0x001F);
    if (doc["colorBandX"].is<int>())
        settings_.colorBandX = sanitizeRgb565Color(doc["colorBandX"], 0x07E0);
    if (doc["colorBandPhoto"].is<int>())
        settings_.colorBandPhoto = sanitizeRgb565Color(doc["colorBandPhoto"], 0x780F);
    if (doc["colorWiFiIcon"].is<int>())
        settings_.colorWiFiIcon = sanitizeRgb565Color(doc["colorWiFiIcon"], 0x07FF);
    if (doc["colorWiFiConnected"].is<int>())
        settings_.colorWiFiConnected = sanitizeRgb565Color(doc["colorWiFiConnected"], 0x07E0);
    if (doc["colorBleConnected"].is<int>())
        settings_.colorBleConnected = sanitizeRgb565Color(doc["colorBleConnected"], 0x07E0);
    if (doc["colorBleDisconnected"].is<int>())
        settings_.colorBleDisconnected = sanitizeRgb565Color(doc["colorBleDisconnected"], 0x001F);
    if (doc["colorBar1"].is<int>())
        settings_.colorBar1 = sanitizeRgb565Color(doc["colorBar1"], 0x07E0);
    if (doc["colorBar2"].is<int>())
        settings_.colorBar2 = sanitizeRgb565Color(doc["colorBar2"], 0x07E0);
    if (doc["colorBar3"].is<int>())
        settings_.colorBar3 = sanitizeRgb565Color(doc["colorBar3"], 0xFFE0);
    if (doc["colorBar4"].is<int>())
        settings_.colorBar4 = sanitizeRgb565Color(doc["colorBar4"], 0xFFE0);
    if (doc["colorBar5"].is<int>())
        settings_.colorBar5 = sanitizeRgb565Color(doc["colorBar5"], 0xF800);
    if (doc["colorBar6"].is<int>())
        settings_.colorBar6 = sanitizeRgb565Color(doc["colorBar6"], 0xF800);
    if (doc["colorMuted"].is<int>())
        settings_.colorMuted = sanitizeRgb565Color(doc["colorMuted"], 0x3186);
    if (doc["colorPersisted"].is<int>())
        settings_.colorPersisted = sanitizeRgb565Color(doc["colorPersisted"], 0x18C3);
    if (doc["colorVolumeMain"].is<int>())
        settings_.colorVolumeMain = sanitizeRgb565Color(doc["colorVolumeMain"], 0xF800);
    if (doc["colorVolumeMute"].is<int>())
        settings_.colorVolumeMute = sanitizeRgb565Color(doc["colorVolumeMute"], 0x7BEF);
    if (doc["colorRssiV1"].is<int>())
        settings_.colorRssiV1 = sanitizeRgb565Color(doc["colorRssiV1"], 0x07E0);
    if (doc["colorRssiProxy"].is<int>())
        settings_.colorRssiProxy = sanitizeRgb565Color(doc["colorRssiProxy"], 0x001F);
    if (doc["colorObd"].is<int>())
        settings_.colorObd = sanitizeRgb565Color(doc["colorObd"], 0x001F);
    if (doc["colorAlpConnected"].is<int>())
        settings_.colorAlpConnected = sanitizeRgb565Color(doc["colorAlpConnected"], 0x07E0);
    if (doc["colorAlpDli"].is<int>())
        settings_.colorAlpDli = sanitizeRgb565Color(doc["colorAlpDli"], 0xFD20);
    if (doc["colorAlpLidActive"].is<int>())
        settings_.colorAlpLidActive = sanitizeRgb565Color(doc["colorAlpLidActive"], 0x001F);
    if (doc["colorAlpAlert"].is<int>())
        settings_.colorAlpAlert = sanitizeRgb565Color(doc["colorAlpAlert"], 0xF800);
    restoreBool("freqUseBandColor", settings_.freqUseBandColor);

    // ============================================================================
    // UI Toggles
    // ============================================================================
    restoreBool("hideWifiIcon", settings_.hideWifiIcon);
    restoreBool("hideProfileIndicator", settings_.hideProfileIndicator);
    restoreBool("hideBatteryIcon", settings_.hideBatteryIcon);
    restoreBool("showBatteryPercent", settings_.showBatteryPercent);
    restoreBool("hideBleIcon", settings_.hideBleIcon);
    restoreBool("hideVolumeIndicator", settings_.hideVolumeIndicator);
    restoreBool("hideRssiIndicator", settings_.hideRssiIndicator);
    restoreBool("alertVolumeFadeEnabled", settings_.alertVolumeFadeEnabled);
    if (doc["alertVolumeFadeDelaySec"].is<int>()) {
        settings_.alertVolumeFadeDelaySec = clampU8(doc["alertVolumeFadeDelaySec"].as<int>(), 1, 10);
    }
    if (doc["alertVolumeFadeVolume"].is<int>()) {
        settings_.alertVolumeFadeVolume = clampU8(doc["alertVolumeFadeVolume"].as<int>(), 1, 9);
    }
    restoreBool("speedMuteEnabled", settings_.speedMuteEnabled);
    if (doc["speedMuteThresholdMph"].is<int>()) {
        settings_.speedMuteThresholdMph = clampU8(doc["speedMuteThresholdMph"].as<int>(), 5, 60);
    }
    if (doc["speedMuteHysteresisMph"].is<int>()) {
        settings_.speedMuteHysteresisMph = clampU8(doc["speedMuteHysteresisMph"].as<int>(), 1, 10);
    }
    if (doc["speedMuteVolume"].is<int>()) {
        int raw = doc["speedMuteVolume"].as<int>();
        if (raw == 255) {
            settings_.speedMuteVolume = 0;
        } else {
            settings_.speedMuteVolume = (raw >= 0 && raw <= 9) ? static_cast<uint8_t>(raw) : 0;
        }
    }
    restoreBool("stealthEnabled", settings_.stealthEnabled);

    // ============================================================================
    // Auto-Push Settings
    // ============================================================================
    restoreBool("autoPushEnabled", settings_.autoPushEnabled);
    if (doc["activeSlot"].is<int>())
        settings_.activeSlot = std::max(0, std::min(doc["activeSlot"].as<int>(), 2));

    if (doc["slot0Name"].is<const char*>())
        settings_.slot0Name = sanitizeSlotNameValue(doc["slot0Name"].as<String>());
    if (doc["slot0Color"].is<int>())
        settings_.slot0Color = sanitizeRgb565Color(doc["slot0Color"], 0x400A);
    if (doc["slot0Volume"].is<int>())
        settings_.slot0Volume = clampSlotVolumeValue(doc["slot0Volume"].as<int>());
    if (doc["slot0MuteVolume"].is<int>())
        settings_.slot0MuteVolume = clampSlotVolumeValue(doc["slot0MuteVolume"].as<int>());
    restoreBool("slot0DarkMode", settings_.slot0DarkMode);
    restoreBool("slot0MuteToZero", settings_.slot0MuteToZero);
    if (doc["slot0AlertPersist"].is<int>())
        settings_.slot0AlertPersist = clampU8(doc["slot0AlertPersist"].as<int>(), 0, 5);
    restoreBool("slot0PriorityArrow", settings_.slot0PriorityArrow);
    if (doc["slot0ProfileName"].is<const char*>())
        settings_.slot0_default.profileName = sanitizeProfileNameValue(doc["slot0ProfileName"].as<String>());
    if (doc["slot0Mode"].is<int>())
        settings_.slot0_default.mode = normalizeV1ModeValue(doc["slot0Mode"].as<int>());

    if (doc["slot1Name"].is<const char*>())
        settings_.slot1Name = sanitizeSlotNameValue(doc["slot1Name"].as<String>());
    if (doc["slot1Color"].is<int>())
        settings_.slot1Color = sanitizeRgb565Color(doc["slot1Color"], 0x07E0);
    if (doc["slot1Volume"].is<int>())
        settings_.slot1Volume = clampSlotVolumeValue(doc["slot1Volume"].as<int>());
    if (doc["slot1MuteVolume"].is<int>())
        settings_.slot1MuteVolume = clampSlotVolumeValue(doc["slot1MuteVolume"].as<int>());
    restoreBool("slot1DarkMode", settings_.slot1DarkMode);
    restoreBool("slot1MuteToZero", settings_.slot1MuteToZero);
    if (doc["slot1AlertPersist"].is<int>())
        settings_.slot1AlertPersist = clampU8(doc["slot1AlertPersist"].as<int>(), 0, 5);
    restoreBool("slot1PriorityArrow", settings_.slot1PriorityArrow);
    if (doc["slot1ProfileName"].is<const char*>())
        settings_.slot1_highway.profileName = sanitizeProfileNameValue(doc["slot1ProfileName"].as<String>());
    if (doc["slot1Mode"].is<int>())
        settings_.slot1_highway.mode = normalizeV1ModeValue(doc["slot1Mode"].as<int>());

    if (doc["slot2Name"].is<const char*>())
        settings_.slot2Name = sanitizeSlotNameValue(doc["slot2Name"].as<String>());
    if (doc["slot2Color"].is<int>())
        settings_.slot2Color = sanitizeRgb565Color(doc["slot2Color"], 0x8410);
    if (doc["slot2Volume"].is<int>())
        settings_.slot2Volume = clampSlotVolumeValue(doc["slot2Volume"].as<int>());
    if (doc["slot2MuteVolume"].is<int>())
        settings_.slot2MuteVolume = clampSlotVolumeValue(doc["slot2MuteVolume"].as<int>());
    restoreBool("slot2DarkMode", settings_.slot2DarkMode);
    restoreBool("slot2MuteToZero", settings_.slot2MuteToZero);
    if (doc["slot2AlertPersist"].is<int>())
        settings_.slot2AlertPersist = clampU8(doc["slot2AlertPersist"].as<int>(), 0, 5);
    restoreBool("slot2PriorityArrow", settings_.slot2PriorityArrow);
    if (doc["slot2ProfileName"].is<const char*>())
        settings_.slot2_comfort.profileName = sanitizeProfileNameValue(doc["slot2ProfileName"].as<String>());
    if (doc["slot2Mode"].is<int>())
        settings_.slot2_comfort.mode = normalizeV1ModeValue(doc["slot2Mode"].as<int>());

    // ============================================================================
    // OBD Settings
    // ============================================================================
    restoreBool("obdEnabled", settings_.obdEnabled);
    if (doc["obdSavedAddress"].is<const char*>()) {
        String addr = doc["obdSavedAddress"].as<String>();
        if (isValidBleAddress(addr)) {
            settings_.obdSavedAddress = addr;
        } else {
            Serial.printf("[Settings] WARN: Invalid OBD saved address in backup: '%s' — skipping\n", addr.c_str());
            settings_.obdSavedAddress = "";
        }
    }
    if (doc["obdSavedName"].is<const char*>())
        settings_.obdSavedName = sanitizeObdSavedNameValue(doc["obdSavedName"].as<String>());
    if (doc["obdSavedAddrType"].is<int>()) {
        settings_.obdSavedAddrType = static_cast<uint8_t>(std::max(0, std::min(doc["obdSavedAddrType"].as<int>(), 1)));
    }
    if (doc["obdMinRssi"].is<int>()) {
        const int rssi = doc["obdMinRssi"].as<int>();
        // Clamp matches the live /api/obd/config handler (-90..-40) so a
        // restored value can always be produced by the API as well.
        settings_.obdMinRssi = static_cast<int8_t>(std::max(-90, std::min(rssi, -40)));
    }
    if (doc["obdScanWindowMs"].is<int>()) {
        settings_.obdScanWindowMs = clampConnectionCycleObdScanWindowMsValue(doc["obdScanWindowMs"].as<int>());
    }
    if (doc["obdRetryIntervalMs"].is<int>()) {
        settings_.obdRetryIntervalMs = clampConnectionCycleObdRetryIntervalMsValue(doc["obdRetryIntervalMs"].as<int>());
    }
    if (doc["proxyOpenWindowMs"].is<int>()) {
        settings_.proxyOpenWindowMs = clampConnectionCycleProxyOpenWindowMsValue(doc["proxyOpenWindowMs"].as<int>());
    }
    if (doc["wifiOpenTimeoutMs"].is<int>()) {
        settings_.wifiOpenTimeoutMs = clampConnectionCycleWifiOpenTimeoutMsValue(doc["wifiOpenTimeoutMs"].as<int>());
    }
    if (doc["v1SettleQuietMs"].is<int>()) {
        settings_.v1SettleQuietMs = clampConnectionCycleV1SettleQuietMsValue(doc["v1SettleQuietMs"].as<int>());
    }
    if (doc["v1SettleFallbackMs"].is<int>()) {
        settings_.v1SettleFallbackMs = clampConnectionCycleV1SettleFallbackMsValue(doc["v1SettleFallbackMs"].as<int>());
    }
    if (doc["cycleTeardownAckTimeoutMs"].is<int>()) {
        settings_.cycleTeardownAckTimeoutMs =
            clampConnectionCycleTeardownAckTimeoutMsValue(doc["cycleTeardownAckTimeoutMs"].as<int>());
    }

    // ALP Settings
    // ============================================================================
    restoreBool("alpEnabled", settings_.alpEnabled);
    restoreBool("alpSdLogEnabled", settings_.alpSdLogEnabled);
    if (doc["alpAlertPersistSec"].is<int>()) {
        settings_.alpAlertPersistSec = clampU8(doc["alpAlertPersistSec"].as<int>(), 0, 5);
    }
    restoreBool("alpDisableV1LaserOnPush", settings_.alpDisableV1LaserOnPush);

    // GPS Settings
    // ============================================================================
    restoreBool("gpsEnabled", settings_.gpsEnabled);
    if (doc["gpsBaud"].is<uint32_t>() || doc["gpsBaud"].is<int>()) {
        settings_.gpsBaud = sanitizeGpsBaudValue(static_cast<uint32_t>(doc["gpsBaud"].as<int>()));
    }
    // Retired compatibility field: GPS EN is not driven on supported hardware.
    settings_.gpsEnablePinActiveHigh = true;
    restoreBool("gpsLogUtcToPerf", settings_.gpsLogUtcToPerf);
    restoreBool("gpsLogUtcToAlp", settings_.gpsLogUtcToAlp);

    // Debug / diagnostics
    restoreBool("powerOffSdLog", settings_.powerOffSdLog);

    if (settings_.proxyBLE && settings_.obdEnabled) {
        // Legacy backups can contain both from the pre-mode era. OBD required
        // a deliberate user opt-in while proxy historically defaulted on, so
        // keep OBD and disable proxy when restoring ambiguous state.
        Serial.println("[Settings] HEAL: restored proxyBLE+obdEnabled — keeping OBD, disabling proxy");
        settings_.proxyBLE = false;
    }

    // Phase 2 done: every scalar/field restore has been applied in RAM.
    feedWatchdog();

    int profilesRestored = 0;
    if (v1ProfileManager.isReady() && doc["profiles"].is<JsonArrayConst>()) {
        JsonArrayConst profilesArr = doc["profiles"].as<JsonArrayConst>();
        int profilesProcessed = 0;
        for (JsonObjectConst p : profilesArr) {
            // Phase 3: one feed per batch of entries, not per entry and not per
            // field.  Placed before the batch so the feed covers the writes that
            // follow it.
            if (++profilesProcessed % kProfileRestoreWatchdogFeedInterval == 0) {
                feedWatchdog();
            }
            if (!p["name"].is<const char*>() || !p["bytes"].is<JsonArrayConst>()) {
                continue;
            }

            JsonArrayConst bytes = p["bytes"].as<JsonArrayConst>();
            if (bytes.size() != 6) {
                continue;
            }

            V1Profile profile;
            profile.name = sanitizeProfileNameValue(p["name"].as<String>());
            if (profile.name.length() == 0) {
                continue;
            }
            if (p["description"].is<const char*>()) {
                profile.description = sanitizeProfileDescriptionValue(p["description"].as<String>());
            }
            bool profileDisplayOn = false;
            if (parseBoolVariant(p["displayOn"], profileDisplayOn)) {
                profile.displayOn = profileDisplayOn;
            }
            if (p["mainVolume"].is<int>())
                profile.mainVolume = clampSlotVolumeValue(p["mainVolume"].as<int>());
            if (p["mutedVolume"].is<int>())
                profile.mutedVolume = clampSlotVolumeValue(p["mutedVolume"].as<int>());

            for (int i = 0; i < 6; i++) {
                profile.settings.bytes[i] = bytes[i].as<uint8_t>();
            }

            ProfileSaveResult saveResult = v1ProfileManager.saveProfile(profile);
            if (saveResult.success) {
                profilesRestored++;
            } else {
                Serial.printf("[Settings] Failed to restore profile '%s': %s\n", profile.name.c_str(),
                              saveResult.error.c_str());
            }
        }
    }

    const bool wasRestorePending = restorePending_;
    clearRestorePending();

    // Phase 4 done: profile writes are finished and the A/B NVS rewrite below is
    // about to start with a full watchdog window in front of it.  The rewrite
    // itself lives in persistSettingsAtomically()/saveDeferredBackup() and is
    // not instrumented here.
    feedWatchdog();

    if (deferBackupRewrite) {
        if (!saveDeferredBackup()) {
            restorePending_ = wasRestorePending;
            Serial.println("[Settings] ERROR: Failed to persist restored settings_");
            return result;
        }
    } else {
        if (!persistSettingsAtomically()) {
            restorePending_ = wasRestorePending;
            Serial.println("[Settings] ERROR: Failed to persist restored settings_");
            return result;
        }
        bumpBackupRevision();
    }

    // Phase 5 done: persist finished; hand the caller a fresh window to build
    // and send its response on.
    feedWatchdog();

    result.success = true;
    result.profilesRestored = profilesRestored;
    return result;
}

bool backupFieldMatchesBool(const JsonDocument& doc, const char* key, bool expected) {
    bool parsed = false;
    return parseBoolVariant(doc[key], parsed) && parsed == expected;
}

bool backupFieldMatchesInt(const JsonDocument& doc, const char* key, int expected) {
    return doc[key].is<int>() && doc[key].as<int>() == expected;
}

bool backupFieldMatchesString(const JsonDocument& doc, const char* key, const String& expected) {
    return doc[key].is<const char*>() && String(doc[key].as<const char*>()) == expected;
}

bool backupAppearsInSyncWithNvs(const JsonDocument& doc, const V1Settings& current) {
    // Core fields that should track one-for-one between healthy NVS and SD backup.
    return backupFieldMatchesBool(doc, "enableWifi", current.enableWifi) &&
           backupFieldMatchesBool(doc, "wifiClientEnabled", current.wifiClientEnabled) &&
           backupFieldMatchesString(doc, "wifiClientSSID", current.wifiClientSSID) &&
           backupFieldMatchesBool(doc, "proxyBLE", current.proxyBLE) &&
           backupFieldMatchesString(doc, "proxyName", current.proxyName) &&
           backupFieldMatchesInt(doc, "brightness", current.brightness) &&
           backupFieldMatchesBool(doc, "autoPushEnabled", current.autoPushEnabled) &&
           backupFieldMatchesInt(doc, "activeSlot", current.activeSlot) &&
           backupFieldMatchesString(doc, "slot0ProfileName", current.slot0_default.profileName) &&
           backupFieldMatchesInt(doc, "slot0Mode", current.slot0_default.mode) &&
           backupFieldMatchesString(doc, "slot1ProfileName", current.slot1_highway.profileName) &&
           backupFieldMatchesInt(doc, "slot1Mode", current.slot1_highway.mode) &&
           backupFieldMatchesString(doc, "slot2ProfileName", current.slot2_comfort.profileName) &&
           backupFieldMatchesInt(doc, "slot2Mode", current.slot2_comfort.mode) &&
           backupFieldMatchesInt(doc, "obdScanWindowMs", static_cast<int>(current.obdScanWindowMs)) &&
           backupFieldMatchesInt(doc, "obdRetryIntervalMs", static_cast<int>(current.obdRetryIntervalMs)) &&
           backupFieldMatchesInt(doc, "proxyOpenWindowMs", static_cast<int>(current.proxyOpenWindowMs)) &&
           backupFieldMatchesInt(doc, "wifiOpenTimeoutMs", static_cast<int>(current.wifiOpenTimeoutMs)) &&
           backupFieldMatchesInt(doc, "v1SettleQuietMs", static_cast<int>(current.v1SettleQuietMs)) &&
           backupFieldMatchesInt(doc, "v1SettleFallbackMs", static_cast<int>(current.v1SettleFallbackMs)) &&
           backupFieldMatchesInt(doc, "cycleTeardownAckTimeoutMs", static_cast<int>(current.cycleTeardownAckTimeoutMs));
}

WifiClientKeyPresence readWifiClientKeyPresence(const char* settingsNamespace) {
    WifiClientKeyPresence presence;
    if (!settingsNamespace || settingsNamespace[0] == '\0') {
        return presence;
    }

    Preferences prefs;
    if (!prefs.begin(settingsNamespace, true)) {
        return presence;
    }
    presence.enabledKeyPresent = prefs.isKey(kNvsWifiClientEnabled);
    presence.ssidKeyPresent = prefs.isKey(kNvsWifiStaSlotSsid[0]) || prefs.isKey(kNvsWifiClientSsid);
    prefs.end();
    return presence;
}

WifiClientSecretPresence readWifiClientSecretPresence(fs::FS* fs) {
    WifiClientSecretPresence presence;
    if (!fs || !fs->exists(WIFI_CLIENT_SD_SECRET_PATH)) {
        return presence;
    }

    File file = fs->open(WIFI_CLIENT_SD_SECRET_PATH, FILE_READ);
    if (!file) {
        return presence;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, file);
    file.close();
    if (err) {
        Serial.printf("[Settings] WARN: Failed to parse WiFi secret '%s': %s\n", WIFI_CLIENT_SD_SECRET_PATH,
                      err.c_str());
        return presence;
    }

    const char* type = doc["_type"] | "";
    if (strcmp(type, WIFI_CLIENT_SD_SECRET_TYPE) != 0) {
        return presence;
    }

    if (doc["secrets"].is<JsonArrayConst>()) {
        JsonArrayConst secrets = doc["secrets"].as<JsonArrayConst>();
        for (JsonObjectConst entry : secrets) {
            if (!entry["ssid"].is<const char*>()) {
                continue;
            }
            const String secretSsid = sanitizeWifiClientSsidValue(entry["ssid"].as<String>());
            if (secretSsid.length() == 0) {
                continue;
            }
            presence.valid = true;
            presence.ssid = secretSsid;
            return presence;
        }
    }

    const char* secretSsid = doc["ssid"] | "";
    if (!secretSsid || secretSsid[0] == '\0') {
        return presence;
    }

    presence.valid = true;
    presence.ssid = sanitizeWifiClientSsidValue(String(secretSsid));
    return presence;
}
