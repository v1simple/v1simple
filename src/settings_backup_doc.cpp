/**
 * Settings backup-document parsing and application.
 */

#include <cstdio>

#include "display_visual_contract.h"
#include "settings_backup_doc.h"
#include "v1_settings_json.h"

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
        Serial.println("[Settings] WARN: Skipping WiFi client password restore; SSID mismatch");
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
        Serial.println("[Settings] WARN: Skipping legacy station password restore; SSID mismatch");
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

void clearWifiStaSlotPasswordsForRestore(StorageManager& storage, bool clearSdSecret) {
    Preferences prefs;
    if (prefs.begin(WIFI_CLIENT_NS, false)) {
        for (size_t i = 0; i < kWifiStaSlotCount; ++i) {
            if (prefs.isKey(kNvsWifiStaSlotPassword[i])) {
                prefs.remove(kNvsWifiStaSlotPassword[i]);
            }
        }
        if (prefs.isKey(kNvsWifiPassword)) {
            prefs.remove(kNvsWifiPassword);
        }
        prefs.end();
    }
    if (clearSdSecret) {
        clearWifiClientSecretFromSD(storage);
    }
}

bool restoreWifiStaSlotsFromBackupDoc(const JsonDocument& doc, V1Settings& settings, StorageManager& storage,
                                      bool clearSdSecret) {
    if (!doc["wifiStaSlots"].is<JsonArrayConst>()) {
        return false;
    }
    bool parsedWifiClientEnabled = false;
    const bool wifiClientEnabledExplicit = parseBoolVariant(doc["wifiClientEnabled"], parsedWifiClientEnabled);

    StoredSlotPasswordSnapshot storedPasswords[kWifiStaSlotCount];
    snapshotWifiStaSlotPasswords(settings, storedPasswords);

    clearWifiStaSlotPasswordsForRestore(storage, clearSdSecret);

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

namespace {

void restoreBackupBool(const JsonDocument& doc, const char* key, bool& target) {
    bool parsed = false;
    if (parseBoolVariant(doc[key], parsed)) target = parsed;
}

struct BackupColorField {
    const char* key;
    uint16_t V1Settings::*target;
    uint16_t fallback;
    bool critical;
};

void applyBackupSignalBarColors(const JsonDocument& doc, V1Settings& settings) {
    const int backupVersion = backupDocumentVersion(doc);
    bool haveDirect = false;
    for (int i = 0; i < 6; ++i) {
        char key[16];
        std::snprintf(key, sizeof(key), "colorBar%d", i + 1);
        haveDirect = haveDirect || doc[key].is<int>();
    }
    bool haveSegments = false;
    for (int i = 0; i < 8; ++i) {
        char key[16];
        std::snprintf(key, sizeof(key), "colorBarS%d", i + 1);
        haveSegments = haveSegments || doc[key].is<int>();
    }
    static constexpr uint16_t defaults[6] = {0x07E0, 0x07E0, 0xFFE0, 0xFFE0, 0xF800, 0xF800};
    if ((backupVersion >= 19 || !haveSegments) && haveDirect) {
        for (int i = 0; i < 6; ++i) {
            char key[16];
            std::snprintf(key, sizeof(key), "colorBar%d", i + 1);
            if (doc[key].is<int>()) settings.colorBars[i] = sanitizeRgb565Color(doc[key], defaults[i]);
        }
    } else if (haveSegments) {
        uint16_t segments[8];
        DisplayVisualContract::expandSixBarColorsToEight(settings.colorBars, segments);
        for (int i = 0; i < 8; ++i) {
            char key[16];
            std::snprintf(key, sizeof(key), "colorBarS%d", i + 1);
            if (doc[key].is<int>()) segments[i] = sanitizeRgb565Color(doc[key], segments[i]);
        }
        DisplayVisualContract::collapseEightBarColorsToSix(segments, settings.colorBars);
    }
}

} // namespace

void applyBackupNetworkFields(const JsonDocument& doc, V1Settings& settings, StorageManager& storage,
                              BackupRestoreScope scope, bool clearSdSecret) {
    if (doc["apPassword"].is<const char*>()) {
        const String decoded = decodeObfuscatedFromStorage(doc["apPassword"].as<String>());
        if (decoded.length() >= MIN_AP_PASSWORD_LEN) settings.apPassword = sanitizeApPasswordValue(decoded);
    }
    if (doc["apSSID"].is<const char*>()) settings.apSSID = sanitizeApSsidValue(doc["apSSID"].as<String>());
    bool clientEnabled = false;
    const bool enabledExplicit = parseBoolVariant(doc["wifiClientEnabled"], clientEnabled);
    if (enabledExplicit) settings.wifiClientEnabled = clientEnabled;
    const bool restoredSlots = restoreWifiStaSlotsFromBackupDoc(doc, settings, storage, clearSdSecret);
    const String legacySsid = legacyWifiClientSsidFromBackupDoc(doc);
    if (!restoredSlots && legacySsid.length() > 0) {
        clearWifiStaSlotPasswordsForRestore(storage, clearSdSecret);
        for (WifiStaSlot& slot : settings.wifiStaSlots) slot = WifiStaSlot();
        settings.wifiStaSlots[0].ssid = legacySsid;
        settings.wifiStaSlots[0].label = "Saved";
        settings.wifiStaSlots[0].priority = 0;
    }
    if (!settings.wifiClientEnabled && settings.hasConfiguredWifiStaSlot() && !enabledExplicit)
        settings.wifiClientEnabled = true;
    settings.refreshWifiClientAliasFromSlots();
    restoreWifiClientPasswordObfFromBackupDoc(doc, settings.wifiClientSSID);
    restoreLegacyStationPasswordFromBackupDoc(doc, settings.wifiClientSSID);

    if (scope == BackupRestoreScope::Full) {
        restoreBackupBool(doc, "proxyBLE", settings.proxyBLE);
        if (doc["proxyName"].is<const char*>()) settings.proxyName = sanitizeProxyNameValue(doc["proxyName"].as<String>());
        if (doc["lastV1Address"].is<const char*>()) settings.lastV1Address = sanitizeLastV1AddressValue(doc["lastV1Address"].as<String>());
        if (doc["autoPowerOffMinutes"].is<int>()) settings.autoPowerOffMinutes = clampU8(doc["autoPowerOffMinutes"], 0, 60);
        if (doc["apTimeoutMinutes"].is<int>()) settings.apTimeoutMinutes = clampApTimeoutValue(doc["apTimeoutMinutes"]);
    }
}

void applyBackupDisplayFields(const JsonDocument& doc, V1Settings& settings, BackupRestoreScope scope) {
    if (doc["brightness"].is<int>()) settings.brightness = clampU8(doc["brightness"], 1, 255);
    static constexpr BackupColorField colors[] = {
        {"colorBogey", &V1Settings::colorBogey, 0xF800, true},
        {"colorFrequency", &V1Settings::colorFrequency, 0xF800, true},
        {"colorArrowFront", &V1Settings::colorArrowFront, 0xF800, true},
        {"colorArrowSide", &V1Settings::colorArrowSide, 0xF800, true},
        {"colorArrowRear", &V1Settings::colorArrowRear, 0xF800, true},
        {"colorBandL", &V1Settings::colorBandL, 0x001F, true},
        {"colorBandKa", &V1Settings::colorBandKa, 0xF800, true},
        {"colorBandK", &V1Settings::colorBandK, 0x001F, true},
        {"colorBandX", &V1Settings::colorBandX, 0x07E0, true},
        {"colorBandPhoto", &V1Settings::colorBandPhoto, 0x780F, true},
        {"colorWiFiConnected", &V1Settings::colorWiFiConnected, 0x07E0, false},
        {"colorBleConnected", &V1Settings::colorBleConnected, 0x07E0, false},
        {"colorBleDisconnected", &V1Settings::colorBleDisconnected, 0x001F, false},
        {"colorMuted", &V1Settings::colorMuted, 0x3186, false},
        {"colorPersisted", &V1Settings::colorPersisted, 0x18C3, false},
        {"colorVolumeMain", &V1Settings::colorVolumeMain, 0xF800, false},
        {"colorVolumeMute", &V1Settings::colorVolumeMute, 0x7BEF, false},
        {"colorRssiV1", &V1Settings::colorRssiV1, 0x07E0, false},
        {"colorRssiProxy", &V1Settings::colorRssiProxy, 0x001F, false},
        {"colorObd", &V1Settings::colorObd, 0x001F, true},
        {"colorAlpConnected", &V1Settings::colorAlpConnected, 0x07E0, true},
        {"colorAlpDli", &V1Settings::colorAlpDli, 0xFD20, true},
        {"colorAlpLidActive", &V1Settings::colorAlpLidActive, 0x001F, true},
        {"colorAlpAlert", &V1Settings::colorAlpAlert, 0xF800, true},
    };
    for (const BackupColorField& field : colors) {
        if ((field.critical || scope == BackupRestoreScope::Full) && doc[field.key].is<int>())
            settings.*(field.target) = sanitizeRgb565Color(doc[field.key], field.fallback);
    }
    if (scope == BackupRestoreScope::Full && !doc["colorWiFiConnected"].is<int>() && doc["colorWiFiIcon"].is<int>())
        settings.colorWiFiConnected = sanitizeRgb565Color(doc["colorWiFiIcon"], 0x07E0);
    if (scope == BackupRestoreScope::Full) {
        applyBackupSignalBarColors(doc, settings);
        restoreBackupBool(doc, "freqUseBandColor", settings.freqUseBandColor);
    }
    static constexpr const char* uiFlags[] = {"hideWifiIcon", "hideProfileIndicator", "hideBatteryIcon",
                                               "showBatteryPercent", "hideBleIcon", "hideVolumeIndicator",
                                               "hideRssiIndicator"};
    bool* uiTargets[] = {&settings.hideWifiIcon, &settings.hideProfileIndicator, &settings.hideBatteryIcon,
                         &settings.showBatteryPercent, &settings.hideBleIcon, &settings.hideVolumeIndicator,
                         &settings.hideRssiIndicator};
    for (size_t i = 0; i < sizeof(uiFlags) / sizeof(uiFlags[0]); ++i)
        restoreBackupBool(doc, uiFlags[i], *uiTargets[i]);
}

void applyBackupAudioFields(const JsonDocument& doc, V1Settings& settings, BackupRestoreScope scope) {
    if (scope == BackupRestoreScope::Full) {
        if (doc["voiceAlertMode"].is<int>()) settings.voiceAlertMode = clampVoiceAlertModeValue(doc["voiceAlertMode"]);
        restoreBackupBool(doc, "voiceDirectionEnabled", settings.voiceDirectionEnabled);
        restoreBackupBool(doc, "announceBogeyCount", settings.announceBogeyCount);
        restoreBackupBool(doc, "muteVoiceIfVolZero", settings.muteVoiceIfVolZero);
        if (doc["voiceVolume"].is<int>()) settings.voiceVolume = clampU8(doc["voiceVolume"], 0, 100);
        restoreBackupBool(doc, "announceSecondaryAlerts", settings.announceSecondaryAlerts);
        restoreBackupBool(doc, "secondaryLaser", settings.secondaryLaser);
        restoreBackupBool(doc, "secondaryKa", settings.secondaryKa);
        restoreBackupBool(doc, "secondaryK", settings.secondaryK);
        restoreBackupBool(doc, "secondaryX", settings.secondaryX);
        restoreBackupBool(doc, "alertVolumeFadeEnabled", settings.alertVolumeFadeEnabled);
        if (doc["alertVolumeFadeDelaySec"].is<int>()) settings.alertVolumeFadeDelaySec = clampU8(doc["alertVolumeFadeDelaySec"], 1, 10);
        if (doc["alertVolumeFadeVolume"].is<int>()) settings.alertVolumeFadeVolume = clampU8(doc["alertVolumeFadeVolume"], 1, 9);
        restoreBackupBool(doc, "speedMuteVoice", settings.speedMuteVoice);
    }
    restoreBackupBool(doc, "speedMuteEnabled", settings.speedMuteEnabled);
    if (doc["speedMuteThresholdMph"].is<int>()) settings.speedMuteThresholdMph = clampU8(doc["speedMuteThresholdMph"], 5, 60);
    if (doc["speedMuteHysteresisMph"].is<int>()) settings.speedMuteHysteresisMph = clampU8(doc["speedMuteHysteresisMph"], 1, 10);
    if (doc["speedMuteVolume"].is<int>()) {
        const int raw = doc["speedMuteVolume"].as<int>();
        settings.speedMuteVolume = (raw >= 0 && raw <= 9) ? static_cast<uint8_t>(raw) : 0;
    }
    restoreBackupBool(doc, "stealthEnabled", settings.stealthEnabled);
}

void applyBackupProfileSlotFields(const JsonDocument& doc, V1Settings& settings, BackupRestoreScope scope) {
    restoreBackupBool(doc, "autoPushEnabled", settings.autoPushEnabled);
    if (doc["activeSlot"].is<int>()) settings.activeSlot = std::max(0, std::min(doc["activeSlot"].as<int>(), 2));
    AutoPushSlot* slots[] = {&settings.slot0_default, &settings.slot1_highway, &settings.slot2_comfort};
    for (int i = 0; i < 3; ++i) {
        char profileKey[24], modeKey[16];
        std::snprintf(profileKey, sizeof(profileKey), "slot%dProfileName", i);
        std::snprintf(modeKey, sizeof(modeKey), "slot%dMode", i);
        if (doc[profileKey].is<const char*>()) slots[i]->profileName = sanitizeProfileNameValue(doc[profileKey].as<String>());
        if (doc[modeKey].is<int>()) slots[i]->mode = normalizeV1ModeValue(doc[modeKey]);
    }
    if (scope != BackupRestoreScope::Full) return;
    String* names[] = {&settings.slot0Name, &settings.slot1Name, &settings.slot2Name};
    uint16_t* colors[] = {&settings.slot0Color, &settings.slot1Color, &settings.slot2Color};
    uint8_t* volumes[] = {&settings.slot0Volume, &settings.slot1Volume, &settings.slot2Volume};
    uint8_t* muteVolumes[] = {&settings.slot0MuteVolume, &settings.slot1MuteVolume, &settings.slot2MuteVolume};
    bool* darkModes[] = {&settings.slot0DarkMode, &settings.slot1DarkMode, &settings.slot2DarkMode};
    bool* muteToZero[] = {&settings.slot0MuteToZero, &settings.slot1MuteToZero, &settings.slot2MuteToZero};
    uint8_t* persists[] = {&settings.slot0AlertPersist, &settings.slot1AlertPersist, &settings.slot2AlertPersist};
    bool* priorityArrows[] = {&settings.slot0PriorityArrow, &settings.slot1PriorityArrow, &settings.slot2PriorityArrow};
    static constexpr uint16_t colorDefaults[] = {0x400A, 0x07E0, 0x8410};
    for (int i = 0; i < 3; ++i) {
        char key[24];
        std::snprintf(key, sizeof(key), "slot%dName", i); if (doc[key].is<const char*>()) *names[i] = sanitizeSlotNameValue(doc[key].as<String>());
        std::snprintf(key, sizeof(key), "slot%dColor", i); if (doc[key].is<int>()) *colors[i] = sanitizeRgb565Color(doc[key], colorDefaults[i]);
        std::snprintf(key, sizeof(key), "slot%dVolume", i); if (doc[key].is<int>()) *volumes[i] = clampSlotVolumeValue(doc[key]);
        std::snprintf(key, sizeof(key), "slot%dMuteVolume", i); if (doc[key].is<int>()) *muteVolumes[i] = clampSlotVolumeValue(doc[key]);
        std::snprintf(key, sizeof(key), "slot%dDarkMode", i); restoreBackupBool(doc, key, *darkModes[i]);
        std::snprintf(key, sizeof(key), "slot%dMuteToZero", i); restoreBackupBool(doc, key, *muteToZero[i]);
        std::snprintf(key, sizeof(key), "slot%dAlertPersist", i); if (doc[key].is<int>()) *persists[i] = clampU8(doc[key], 0, 5);
        std::snprintf(key, sizeof(key), "slot%dPriorityArrow", i); restoreBackupBool(doc, key, *priorityArrows[i]);
        sanitizeSlotVolumePair(*volumes[i], *muteVolumes[i]);
    }
}

void applyBackupObdFields(const JsonDocument& doc, V1Settings& settings, BackupRestoreScope scope) {
    restoreBackupBool(doc, "obdEnabled", settings.obdEnabled);
    if (doc["obdSavedName"].is<const char*>()) settings.obdSavedName = sanitizeObdSavedNameValue(doc["obdSavedName"].as<String>());
    if (scope != BackupRestoreScope::Full) return;
    if (doc["obdSavedAddress"].is<const char*>()) {
        const String address = doc["obdSavedAddress"].as<String>();
        settings.obdSavedAddress = isValidBleAddress(address) ? address : "";
        if (settings.obdSavedAddress.length() == 0 && address.length() > 0)
            Serial.println("[Settings] WARN: Invalid OBD saved address in backup — skipping");
    }
    if (doc["obdSavedAddrType"].is<int>()) settings.obdSavedAddrType = clampU8(doc["obdSavedAddrType"], 0, 1);
    if (doc["obdMinRssi"].is<int>()) settings.obdMinRssi = static_cast<int8_t>(std::max(-90, std::min(doc["obdMinRssi"].as<int>(), -40)));
    if (doc["obdScanWindowMs"].is<int>()) settings.obdScanWindowMs = clampConnectionCycleObdScanWindowMsValue(doc["obdScanWindowMs"]);
    if (doc["obdRetryIntervalMs"].is<int>()) settings.obdRetryIntervalMs = clampConnectionCycleObdRetryIntervalMsValue(doc["obdRetryIntervalMs"]);
    if (doc["proxyOpenWindowMs"].is<int>()) settings.proxyOpenWindowMs = clampConnectionCycleProxyOpenWindowMsValue(doc["proxyOpenWindowMs"]);
    if (doc["v1SettleQuietMs"].is<int>()) settings.v1SettleQuietMs = clampConnectionCycleV1SettleQuietMsValue(doc["v1SettleQuietMs"]);
    if (doc["v1SettleFallbackMs"].is<int>()) settings.v1SettleFallbackMs = clampConnectionCycleV1SettleFallbackMsValue(doc["v1SettleFallbackMs"]);
    if (doc["cycleTeardownAckTimeoutMs"].is<int>()) settings.cycleTeardownAckTimeoutMs = clampConnectionCycleTeardownAckTimeoutMsValue(doc["cycleTeardownAckTimeoutMs"]);
}

void applyBackupAlpAndGpsFields(const JsonDocument& doc, V1Settings& settings) {
    restoreBackupBool(doc, "alpEnabled", settings.alpEnabled);
    if (doc["alpAlertPersistSec"].is<int>()) settings.alpAlertPersistSec = clampU8(doc["alpAlertPersistSec"], 0, 5);
    restoreBackupBool(doc, "alpDisableV1LaserOnPush", settings.alpDisableV1LaserOnPush);
    restoreBackupBool(doc, "gpsEnabled", settings.gpsEnabled);
    if (doc["gpsBaud"].is<uint32_t>() || doc["gpsBaud"].is<int>())
        settings.gpsBaud = sanitizeGpsBaudValue(static_cast<uint32_t>(doc["gpsBaud"].as<int>()));
}

void healBackupRestoreConflicts(V1Settings& settings, const char* context) {
    if (settings.proxyBLE && settings.obdEnabled) {
        Serial.printf("[Settings] HEAL: %s proxyBLE+obdEnabled — keeping OBD, disabling proxy\n", context);
        settings.proxyBLE = false;
    }
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

    applyBackupNetworkFields(doc, settings_, *storage_, BackupRestoreScope::Full, deferBackupRewrite);
    feedWatchdog();

    applyBackupDisplayFields(doc, settings_, BackupRestoreScope::Full);
    applyBackupAudioFields(doc, settings_, BackupRestoreScope::Full);
    applyBackupProfileSlotFields(doc, settings_, BackupRestoreScope::Full);
    applyBackupObdFields(doc, settings_, BackupRestoreScope::Full);
    applyBackupAlpAndGpsFields(doc, settings_);
    healBackupRestoreConflicts(settings_, "restored");
    feedWatchdog();

    int profilesRestored = 0;
    if (profiles_->isReady() && doc["profiles"].is<JsonArrayConst>()) {
        JsonArrayConst profilesArr = doc["profiles"].as<JsonArrayConst>();
        int profilesProcessed = 0;
        for (JsonObjectConst p : profilesArr) {
            // Phase 3: one feed per batch of entries, not per entry and not per
            // field.  Placed before the batch so the feed covers the writes that
            // follow it.
            if (++profilesProcessed % kProfileRestoreWatchdogFeedInterval == 0) {
                feedWatchdog();
            }
            if (!p["name"].is<const char*>()) {
                continue;
            }

            V1Profile profile;
            const JsonVariantConst rawBytes = p["bytes"];
            if (!V1SettingsJson::parseRawBytes(rawBytes, profile.settings.bytes)) {
                continue;
            }
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

            ProfileSaveResult saveResult = profiles_->saveProfile(profile);
            if (saveResult.success) {
                profilesRestored++;
            } else {
                Serial.println("[Settings] Failed to restore one profile");
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
        noteNvsCommitWithoutBackupIntent();
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
    return backupFieldMatchesBool(doc, "wifiClientEnabled", current.wifiClientEnabled) &&
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
