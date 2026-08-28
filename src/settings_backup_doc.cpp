/**
 * Settings backup-document parsing and application.
 */

#include <cstdio>
#include <limits>

#include "backup_payload_builder.h"
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

bool snapshotWifiStaSlotPasswords(const V1Settings& settings,
                                  StoredSlotPasswordSnapshot (&snapshot)[kWifiStaSlotCount]) {
    Preferences prefs;
    const bool opened = prefs.begin(WIFI_CLIENT_NS, true);
    if (!opened) {
        return false;
    }
    for (size_t i = 0; i < kWifiStaSlotCount; ++i) {
        snapshot[i].ssid = settings.wifiStaSlots[i].ssid;
        snapshot[i].passwordObf = "";
        if (snapshot[i].ssid.length() > 0 && prefs.isKey(kNvsWifiStaSlotPassword[i])) {
            snapshot[i].passwordObf = prefs.getString(kNvsWifiStaSlotPassword[i], "");
        }
    }
    prefs.end();
    return true;
}

bool preserveStoredPasswordForMatchingSsid(const String& ssid, size_t targetIndex,
                                           const StoredSlotPasswordSnapshot (&snapshot)[kWifiStaSlotCount],
                                           bool& found) {
    found = false;
    for (size_t i = 0; i < kWifiStaSlotCount; ++i) {
        if (snapshot[i].passwordObf.length() == 0 || snapshot[i].ssid != ssid) {
            continue;
        }
        found = true;
        if (storeWifiClientPasswordObfToNvs(snapshot[i].passwordObf, targetIndex)) {
            Serial.printf("[Settings] Preserved stored WiFi password for slot %u (SSID match)\n",
                          static_cast<unsigned>(targetIndex));
            return true;
        }
        return false;
    }
    return true;
}

bool clearWifiStaSlotPasswordsForRestore(StorageManager& storage, bool clearSdSecret) {
    Preferences prefs;
    if (!prefs.begin(WIFI_CLIENT_NS, false)) {
        return false;
    }
    bool cleared = true;
    for (size_t i = 0; i < kWifiStaSlotCount; ++i) {
        if (prefs.isKey(kNvsWifiStaSlotPassword[i])) {
            cleared = prefs.remove(kNvsWifiStaSlotPassword[i]) &&
                      !prefs.isKey(kNvsWifiStaSlotPassword[i]) && cleared;
        }
    }
    if (prefs.isKey(kNvsWifiPassword)) {
        cleared = prefs.remove(kNvsWifiPassword) && !prefs.isKey(kNvsWifiPassword) && cleared;
    }
    prefs.end();
    if (clearSdSecret) {
        cleared = clearWifiClientSecretFromSD(storage) && cleared;
    }
    return cleared;
}

bool restoreWifiStaSlotsFromBackupDoc(const JsonDocument& doc, V1Settings& settings, StorageManager& storage,
                                      bool clearSdSecret) {
    if (!doc["wifiStaSlots"].is<JsonArrayConst>()) {
        return false;
    }
    bool parsedWifiClientEnabled = false;
    const bool wifiClientEnabledExplicit = parseBoolVariant(doc["wifiClientEnabled"], parsedWifiClientEnabled);

    StoredSlotPasswordSnapshot storedPasswords[kWifiStaSlotCount];
    if (!snapshotWifiStaSlotPasswords(settings, storedPasswords)) {
        return false;
    }

    if (!clearWifiStaSlotPasswordsForRestore(storage, clearSdSecret)) {
        return false;
    }

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
        if (slotObj["passwordObf"].is<const char*>()) {
            if (!restoreWifiStaSlotPasswordObfFromBackupSlot(slotObj, static_cast<size_t>(index))) {
                return false;
            }
        } else {
            // Sanitized backup (no passwordObf): keep the stored password when
            // the incoming slot names a network we already have credentials for.
            bool foundStoredPassword = false;
            if (!preserveStoredPasswordForMatchingSsid(slot.ssid, static_cast<size_t>(index), storedPasswords,
                                                       foundStoredPassword)) {
                return false;
            }
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

bool applyBackupNetworkFields(const JsonDocument& doc, V1Settings& settings, StorageManager& storage,
                              BackupRestoreScope scope, bool clearSdSecret) {
    if (doc["apPassword"].is<const char*>()) {
        const String decoded = decodeObfuscatedFromStorage(doc["apPassword"].as<String>());
        if (decoded.length() >= MIN_AP_PASSWORD_LEN) settings.apPassword = sanitizeApPasswordValue(decoded);
    }
    if (doc["apSSID"].is<const char*>()) settings.apSSID = sanitizeApSsidValue(doc["apSSID"].as<String>());
    bool clientEnabled = false;
    const bool enabledExplicit = parseBoolVariant(doc["wifiClientEnabled"], clientEnabled);
    if (enabledExplicit) settings.wifiClientEnabled = clientEnabled;
    const bool hasSlotDocument = doc["wifiStaSlots"].is<JsonArrayConst>();
    bool restoredSlots = false;
    if (hasSlotDocument) {
        if (!restoreWifiStaSlotsFromBackupDoc(doc, settings, storage, clearSdSecret)) {
            return false;
        }
        restoredSlots = true;
    }
    const String legacySsid = legacyWifiClientSsidFromBackupDoc(doc);
    if (!restoredSlots && legacySsid.length() > 0) {
        if (!clearWifiStaSlotPasswordsForRestore(storage, clearSdSecret)) {
            return false;
        }
        for (WifiStaSlot& slot : settings.wifiStaSlots) slot = WifiStaSlot();
        settings.wifiStaSlots[0].ssid = legacySsid;
        settings.wifiStaSlots[0].label = "Saved";
        settings.wifiStaSlots[0].priority = 0;
    }
    if (!settings.wifiClientEnabled && settings.hasConfiguredWifiStaSlot() && !enabledExplicit)
        settings.wifiClientEnabled = true;
    settings.refreshWifiClientAliasFromSlots();
    if (doc["wifiClientPasswordObf"].is<const char*>() &&
        !restoreWifiClientPasswordObfFromBackupDoc(doc, settings.wifiClientSSID)) {
        return false;
    }
    if (doc["stationPassword"].is<const char*>() &&
        !restoreLegacyStationPasswordFromBackupDoc(doc, settings.wifiClientSSID)) {
        return false;
    }

    if (clearSdSecret && (hasSlotDocument || legacySsid.length() > 0)) {
        if (!storage.isReady() || !storage.isSDCard()) {
            return false;
        }
        Preferences passwordPrefs;
        if (!passwordPrefs.begin(WIFI_CLIENT_NS, true)) {
            return false;
        }
        String encodedPasswords[kWifiStaSlotCount];
        for (size_t index = 0; index < kWifiStaSlotCount; ++index) {
            if (passwordPrefs.isKey(kNvsWifiStaSlotPassword[index])) {
                encodedPasswords[index] = passwordPrefs.getString(kNvsWifiStaSlotPassword[index], "");
            }
        }
        passwordPrefs.end();
        for (size_t index = 0; index < kWifiStaSlotCount; ++index) {
            if (settings.wifiStaSlots[index].isConfigured() && encodedPasswords[index].length() > 0 &&
                !saveWifiClientSecretToSD(storage, index, settings.wifiStaSlots[index].ssid,
                                          encodedPasswords[index])) {
                return false;
            }
        }
    }

    if (scope == BackupRestoreScope::Full) {
        restoreBackupBool(doc, "proxyBLE", settings.proxyBLE);
        if (doc["proxyName"].is<const char*>()) settings.proxyName = sanitizeProxyNameValue(doc["proxyName"].as<String>());
        if (doc["lastV1Address"].is<const char*>()) settings.lastV1Address = sanitizeLastV1AddressValue(doc["lastV1Address"].as<String>());
        if (doc["autoPowerOffMinutes"].is<int>()) settings.autoPowerOffMinutes = clampU8(doc["autoPowerOffMinutes"], 0, 60);
        if (doc["apTimeoutMinutes"].is<int>()) settings.apTimeoutMinutes = clampApTimeoutValue(doc["apTimeoutMinutes"]);
    }
    return true;
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

namespace {

bool parseBackupProfile(JsonObjectConst source, V1Profile& profile) {
    if (!source["name"].is<const char*>() ||
        !V1SettingsJson::parseRawBytes(source["bytes"], profile.settings.bytes)) {
        return false;
    }
    String canonical;
    if (canonicalizeProfileName(source["name"].as<String>(), canonical) != ProfileNameStatus::Valid) {
        return false;
    }
    profile.name = canonical;
    if (!source["description"].isNull()) {
        if (!source["description"].is<const char*>()) {
            return false;
        }
        profile.description = sanitizeProfileDescriptionValue(source["description"].as<String>());
    }
    if (!source["displayOn"].isNull()) {
        bool displayOn = true;
        if (!parseBoolVariant(source["displayOn"], displayOn)) {
            return false;
        }
        profile.displayOn = displayOn;
    }
    const auto readVolume = [&](const char* key, uint8_t& target) {
        if (source[key].isNull()) {
            return true;
        }
        if (!source[key].is<int>()) {
            return false;
        }
        const int value = source[key].as<int>();
        if (!((value >= 0 && value <= 9) || value == 0xFF)) {
            return false;
        }
        target = static_cast<uint8_t>(value);
        return true;
    };
    return readVolume("mainVolume", profile.mainVolume) && readVolume("mutedVolume", profile.mutedVolume);
}

bool validateBackupNetworkCredentialFields(const JsonDocument& doc) {
    bool enabled = false;
    if (!doc["wifiClientEnabled"].isNull() && !parseBoolVariant(doc["wifiClientEnabled"], enabled)) {
        return false;
    }
    if (!doc["wifiStaSlots"].isNull()) {
        if (!doc["wifiStaSlots"].is<JsonArrayConst>()) {
            return false;
        }
        bool seen[kWifiStaSlotCount] = {};
        for (JsonVariantConst value : doc["wifiStaSlots"].as<JsonArrayConst>()) {
            if (!value.is<JsonObjectConst>()) {
                return false;
            }
            JsonObjectConst slot = value.as<JsonObjectConst>();
            if (!slot["index"].is<int>() || !slot["ssid"].is<const char*>()) {
                return false;
            }
            const int rawIndex = slot["index"].as<int>();
            if (rawIndex < 0 || rawIndex >= static_cast<int>(kWifiStaSlotCount) || seen[rawIndex]) {
                return false;
            }
            seen[rawIndex] = true;
            const String ssid = slot["ssid"].as<String>();
            if (ssid.length() == 0 || sanitizeWifiClientSsidValue(ssid) != ssid ||
                (!slot["label"].isNull() && !slot["label"].is<const char*>()) ||
                (!slot["priority"].isNull() && !slot["priority"].is<int>()) ||
                (!slot["lastConnectedAtSec"].isNull() && !slot["lastConnectedAtSec"].is<uint32_t>() &&
                 !slot["lastConnectedAtSec"].is<int>())) {
                return false;
            }
            if (!slot["passwordObf"].isNull()) {
                if (!slot["passwordObf"].is<const char*>()) {
                    return false;
                }
                const String encoded = slot["passwordObf"].as<String>();
                if (encoded.length() == 0 || decodeObfuscatedFromStorage(encoded).length() == 0) {
                    return false;
                }
            }
        }
    }
    if (!doc["wifiClientPasswordObf"].isNull()) {
        if (!doc["wifiClientPasswordObf"].is<const char*>()) {
            return false;
        }
        const String encoded = doc["wifiClientPasswordObf"].as<String>();
        if (encoded.length() == 0 || decodeObfuscatedFromStorage(encoded).length() == 0 ||
            legacyWifiClientSsidFromBackupDoc(doc).length() == 0) {
            return false;
        }
    }
    if (!doc["stationPassword"].isNull() &&
        (!doc["stationPassword"].is<const char*>() ||
         sanitizeWifiClientPasswordValue(doc["stationPassword"].as<String>()).length() == 0 ||
         legacyWifiClientSsidFromBackupDoc(doc).length() == 0)) {
        return false;
    }
    return true;
}

bool validateBackupDocumentForApply(const JsonDocument& doc, const V1Settings& current, V1ProfileManager& profiles,
                                    std::vector<V1Profile>& incomingProfiles,
                                    std::vector<V1Profile>& existingProfiles) {
    if (!doc.is<JsonObjectConst>()) {
        return false;
    }
    if (!doc["_type"].isNull() &&
        (!doc["_type"].is<const char*>() ||
         !BackupPayloadBuilder::isRecognizedBackupType(doc["_type"].as<const char*>()))) {
        return false;
    }
    if (!doc["_crc32"].isNull()) {
        if (!doc["_crc32"].is<uint32_t>() ||
            doc["_crc32"].as<uint32_t>() != BackupPayloadBuilder::computeBackupCrc32(doc)) {
            return false;
        }
    }
    if (!doc["profiles"].isNull() && !doc["profiles"].is<JsonArrayConst>()) {
        return false;
    }
    if (!validateBackupNetworkCredentialFields(doc)) {
        return false;
    }

    std::vector<String> availableNames;
    if (profiles.isReady()) {
        const ProfileOperationResult snapshot = profiles.snapshotProfiles(existingProfiles, 250);
        if (!snapshot.success()) {
            return false;
        }
        availableNames.reserve(existingProfiles.size() +
                               (doc["profiles"].is<JsonArrayConst>() ? doc["profiles"].size() : 0));
        for (const V1Profile& profile : existingProfiles) {
            availableNames.push_back(profileCanonicalCollisionKey(profile.name));
        }
    }

    if (doc["profiles"].is<JsonArrayConst>()) {
        if (!profiles.isReady()) {
            return false;
        }
        for (JsonVariantConst value : doc["profiles"].as<JsonArrayConst>()) {
            if (!value.is<JsonObjectConst>()) {
                return false;
            }
            V1Profile profile;
            if (!parseBackupProfile(value.as<JsonObjectConst>(), profile)) {
                return false;
            }
            const String collisionKey = profileCanonicalCollisionKey(profile.name);
            for (const V1Profile& prior : incomingProfiles) {
                if (profileCanonicalCollisionKey(prior.name) == collisionKey) {
                    return false;
                }
            }
            incomingProfiles.push_back(profile);
            bool alreadyAvailable = false;
            for (const String& available : availableNames) {
                alreadyAvailable |= available == collisionKey;
            }
            if (!alreadyAvailable) {
                availableNames.push_back(collisionKey);
            }
        }
    }

    const String currentAssignments[3] = {current.slot0_default.profileName, current.slot1_highway.profileName,
                                          current.slot2_comfort.profileName};
    for (int slot = 0; slot < 3; ++slot) {
        char key[24];
        std::snprintf(key, sizeof(key), "slot%dProfileName", slot);
        String assigned = currentAssignments[slot];
        if (!doc[key].isNull()) {
            if (!doc[key].is<const char*>()) {
                return false;
            }
            assigned = doc[key].as<String>();
        }
        if (assigned.length() == 0) {
            continue;
        }
        String canonical;
        if (canonicalizeProfileName(assigned, canonical) != ProfileNameStatus::Valid) {
            return false;
        }
        const String collisionKey = profileCanonicalCollisionKey(canonical);
        bool found = false;
        for (const String& available : availableNames) {
            found |= available == collisionKey;
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

bool restoreProfileSnapshot(V1ProfileManager& profiles, const std::vector<V1Profile>& before) {
    std::vector<V1Profile> current;
    if (!profiles.snapshotProfiles(current, 250).success()) {
        return false;
    }
    bool restored = true;
    for (const V1Profile& profile : current) {
        const String key = profileCanonicalCollisionKey(profile.name);
        bool existedBefore = false;
        for (const V1Profile& prior : before) {
            existedBefore |= profileCanonicalCollisionKey(prior.name) == key;
        }
        if (!existedBefore) {
            restored = profiles.deleteProfileResult(profile.name, 250).success() && restored;
        }
    }
    for (const V1Profile& profile : before) {
        restored = profiles.saveProfile(profile).success && restored;
    }
    return restored;
}

struct RestoreCredentialSnapshot {
    bool passwordPresent[kWifiStaSlotCount] = {};
    String passwordValues[kWifiStaSlotCount];
    bool legacyPasswordPresent = false;
    String legacyPasswordValue;
    bool sdRelevant = false;
    bool sdFilePresent = false;
    String sdFileBytes;
};

constexpr const char* RESTORE_TRANSACTION_PATH = "/v1restore_transaction.json";
constexpr const char* RESTORE_TRANSACTION_TMP_PATH = "/v1restore_transaction.tmp";
constexpr const char* RESTORE_TRANSACTION_TYPE = "v1simple_restore_transaction";
constexpr int RESTORE_TRANSACTION_VERSION = 2;
constexpr size_t RESTORE_TRANSACTION_MAX_BYTES = 128 * 1024;

constexpr const char* PROFILE_DELETE_TRANSACTION_PATH = "/v1profile_delete_transaction.json";
constexpr const char* PROFILE_DELETE_TRANSACTION_TMP_PATH = "/v1profile_delete_transaction.tmp";
constexpr const char* PROFILE_DELETE_TRANSACTION_TYPE = "v1simple_profile_delete_transaction";
constexpr int PROFILE_DELETE_TRANSACTION_VERSION = 2;
constexpr size_t PROFILE_DELETE_TRANSACTION_MAX_BYTES = 8 * 1024;

struct RestoreTransactionJournal {
    uint64_t token = 0;
    bool credentialsMutated = false;
    RestoreCredentialSnapshot credentialsBefore;
    bool profilesMutated = false;
    std::vector<V1Profile> profilesBefore;
};

struct ProfileDeleteTransactionJournal {
    uint64_t token = 0;
    bool hadReferences = false;
    V1Profile profile;
};

enum class JournalCopyStatus : uint8_t {
    Missing,
    Invalid,
    Parsed,
    Valid,
};

struct JournalCopy {
    JournalCopyStatus status = JournalCopyStatus::Missing;
    JsonDocument doc;
};

struct JournalMirrors {
    fs::FS* primaryFs = nullptr;
    fs::FS* secondaryFs = nullptr;
    JournalCopy primary;
    JournalCopy secondary;
};

bool jsonHasExactSize(JsonObjectConst object, size_t expected) {
    return object.size() == expected;
}

bool profileSnapshotsEqual(const V1Profile& lhs, const V1Profile& rhs) {
    return lhs.name == rhs.name && lhs.description == rhs.description && lhs.displayOn == rhs.displayOn &&
           lhs.mainVolume == rhs.mainVolume && lhs.mutedVolume == rhs.mutedVolume &&
           memcmp(lhs.settings.bytes, rhs.settings.bytes, sizeof(lhs.settings.bytes)) == 0;
}

bool credentialSnapshotsEqual(const RestoreCredentialSnapshot& lhs, const RestoreCredentialSnapshot& rhs) {
    for (size_t i = 0; i < kWifiStaSlotCount; ++i) {
        if (lhs.passwordPresent[i] != rhs.passwordPresent[i] ||
            (lhs.passwordPresent[i] && lhs.passwordValues[i] != rhs.passwordValues[i])) {
            return false;
        }
    }
    return lhs.legacyPasswordPresent == rhs.legacyPasswordPresent &&
           (!lhs.legacyPasswordPresent || lhs.legacyPasswordValue == rhs.legacyPasswordValue) &&
           lhs.sdRelevant == rhs.sdRelevant && lhs.sdFilePresent == rhs.sdFilePresent &&
           (!(lhs.sdRelevant && lhs.sdFilePresent) || lhs.sdFileBytes == rhs.sdFileBytes);
}

bool restoreJournalsEqual(const RestoreTransactionJournal& lhs, const RestoreTransactionJournal& rhs) {
    if (lhs.token != rhs.token || lhs.credentialsMutated != rhs.credentialsMutated ||
        lhs.profilesMutated != rhs.profilesMutated ||
        (lhs.credentialsMutated && !credentialSnapshotsEqual(lhs.credentialsBefore, rhs.credentialsBefore)) ||
        lhs.profilesBefore.size() != rhs.profilesBefore.size()) {
        return false;
    }
    for (size_t i = 0; i < lhs.profilesBefore.size(); ++i) {
        if (!profileSnapshotsEqual(lhs.profilesBefore[i], rhs.profilesBefore[i])) {
            return false;
        }
    }
    return true;
}

bool deleteJournalsEqual(const ProfileDeleteTransactionJournal& lhs,
                         const ProfileDeleteTransactionJournal& rhs) {
    return lhs.token == rhs.token && lhs.hadReferences == rhs.hadReferences &&
           profileSnapshotsEqual(lhs.profile, rhs.profile);
}

void writeProfileToJournal(JsonObject target, const V1Profile& profile) {
    target["name"] = profile.name;
    target["description"] = profile.description;
    target["displayOn"] = profile.displayOn;
    target["mainVolume"] = profile.mainVolume;
    target["mutedVolume"] = profile.mutedVolume;
    JsonArray bytes = target["bytes"].to<JsonArray>();
    for (uint8_t byte : profile.settings.bytes) {
        bytes.add(byte);
    }
}

bool readProfileFromJournal(JsonObjectConst source, V1Profile& profile) {
    return jsonHasExactSize(source, 6) && source["name"].is<const char*>() &&
           source["description"].is<const char*>() && source["displayOn"].is<bool>() &&
           source["mainVolume"].is<uint8_t>() && source["mutedVolume"].is<uint8_t>() &&
           source["bytes"].is<JsonArrayConst>() && parseBackupProfile(source, profile);
}

void writeCredentialSnapshotToJournal(JsonObject target, const RestoreCredentialSnapshot& snapshot) {
    JsonArray slots = target["slots"].to<JsonArray>();
    for (size_t i = 0; i < kWifiStaSlotCount; ++i) {
        JsonObject slot = slots.add<JsonObject>();
        slot["present"] = snapshot.passwordPresent[i];
        if (snapshot.passwordPresent[i]) {
            slot["value"] = snapshot.passwordValues[i];
        }
    }
    target["legacyPresent"] = snapshot.legacyPasswordPresent;
    if (snapshot.legacyPasswordPresent) {
        target["legacyValue"] = snapshot.legacyPasswordValue;
    }
    target["sdRelevant"] = snapshot.sdRelevant;
    target["sdPresent"] = snapshot.sdFilePresent;
    if (snapshot.sdRelevant && snapshot.sdFilePresent) {
        target["sdBytes"] = snapshot.sdFileBytes;
    }
}

bool readCredentialSnapshotFromJournal(JsonObjectConst source, RestoreCredentialSnapshot& snapshot) {
    if (!source["slots"].is<JsonArrayConst>() || source["slots"].size() != kWifiStaSlotCount ||
        !source["legacyPresent"].is<bool>() || !source["sdRelevant"].is<bool>() ||
        !source["sdPresent"].is<bool>()) {
        return false;
    }
    size_t index = 0;
    for (JsonVariantConst value : source["slots"].as<JsonArrayConst>()) {
        if (!value.is<JsonObjectConst>()) {
            return false;
        }
        JsonObjectConst slot = value.as<JsonObjectConst>();
        if (!slot["present"].is<bool>()) {
            return false;
        }
        snapshot.passwordPresent[index] = slot["present"].as<bool>();
        if (!jsonHasExactSize(slot, snapshot.passwordPresent[index] ? 2 : 1)) {
            return false;
        }
        if (snapshot.passwordPresent[index]) {
            if (!slot["value"].is<const char*>()) {
                return false;
            }
            snapshot.passwordValues[index] = slot["value"].as<String>();
        }
        ++index;
    }
    snapshot.legacyPasswordPresent = source["legacyPresent"].as<bool>();
    if (snapshot.legacyPasswordPresent) {
        if (!source["legacyValue"].is<const char*>()) {
            return false;
        }
        snapshot.legacyPasswordValue = source["legacyValue"].as<String>();
    }
    snapshot.sdRelevant = source["sdRelevant"].as<bool>();
    snapshot.sdFilePresent = source["sdPresent"].as<bool>();
    if (!snapshot.sdRelevant && snapshot.sdFilePresent) {
        return false;
    }
    if (snapshot.sdRelevant && snapshot.sdFilePresent) {
        if (!source["sdBytes"].is<const char*>()) {
            return false;
        }
        snapshot.sdFileBytes = source["sdBytes"].as<String>();
    }
    const size_t expectedKeys = 4 + (snapshot.legacyPasswordPresent ? 1 : 0) +
                                ((snapshot.sdRelevant && snapshot.sdFilePresent) ? 1 : 0);
    return jsonHasExactSize(source, expectedKeys);
}

void stampJournalCrc(JsonDocument& doc) {
    doc.remove("_crc32");
    doc["_crc32"] = BackupPayloadBuilder::computeBackupCrc32(doc);
}

bool journalCrcValid(const JsonDocument& doc) {
    return doc["_crc32"].is<uint32_t>() &&
           doc["_crc32"].as<uint32_t>() == BackupPayloadBuilder::computeBackupCrc32(doc);
}

bool writeJournalAtomically(StorageManager& storage, const char* path, const char* tempPath,
                            size_t maxBytes, const JsonDocument& doc) {
    if (!storage.isReady() || !path || !tempPath) {
        return false;
    }
    StorageManager::SDLockBlocking lock(storage.getSDMutex());
    if (!lock) {
        return false;
    }
    const auto writeOne = [&](fs::FS* fs) {
        if (!fs) {
            return false;
        }
        if (fs->exists(tempPath)) {
            fs->remove(tempPath);
        }
        File file = fs->open(tempPath, FILE_WRITE);
        if (!file) {
            return false;
        }
        const size_t expected = measureJson(doc);
        if (expected == 0 || expected > maxBytes) {
            fs->remove(tempPath);
            return false;
        }
        const size_t written = serializeJson(doc, file);
        file.flush();
        file.close();
        if (written != expected) {
            fs->remove(tempPath);
            return false;
        }
        JsonDocument verified;
        File verify = fs->open(tempPath, FILE_READ);
        const bool valid = verify && verify.size() == written && !deserializeJson(verified, verify) &&
                           journalCrcValid(verified);
        if (verify) {
            verify.close();
        }
        if (!valid || !StorageManager::promoteTempFileWithRollback(*fs, tempPath, path)) {
            fs->remove(tempPath);
            return false;
        }
        return true;
    };

    fs::FS* primary = storage.getFilesystem();
    fs::FS* secondary = storage.getLittleFS();
    if (!writeOne(primary)) {
        return false;
    }
    if (secondary && secondary != primary && !writeOne(secondary)) {
        primary->remove(path);
        return false;
    }
    return true;
}

bool loadJournalMirrors(StorageManager& storage, const char* path, size_t maxBytes, JournalMirrors& mirrors) {
    if (!path || maxBytes == 0) {
        return false;
    }
    if (!storage.isReady()) {
        return true;
    }
    mirrors.primaryFs = storage.getFilesystem();
    mirrors.secondaryFs = storage.getLittleFS();
    if (mirrors.secondaryFs == mirrors.primaryFs) {
        mirrors.secondaryFs = nullptr;
    }
    // Preserve the ordinary no-journal fast path: it neither allocates nor
    // consumes an SD-mutex attempt needed by the caller's real operation.
    if ((!mirrors.primaryFs || !mirrors.primaryFs->exists(path)) &&
        (!mirrors.secondaryFs || !mirrors.secondaryFs->exists(path))) {
        return true;
    }
    StorageManager::SDLockBlocking lock(storage.getSDMutex());
    if (!lock) {
        return false;
    }
    const auto readOne = [&](fs::FS* filesystem, JournalCopy& copy) {
        if (!filesystem || !filesystem->exists(path)) {
            copy.status = JournalCopyStatus::Missing;
            return;
        }
        File file = filesystem->open(path, FILE_READ);
        if (!file || file.size() == 0 || file.size() > maxBytes) {
            if (file) file.close();
            copy.status = JournalCopyStatus::Invalid;
            return;
        }
        const DeserializationError error = deserializeJson(copy.doc, file);
        bool trailingWhitespaceOnly = true;
        while (file.available() > 0) {
            const int value = file.read();
            if (value < 0) {
                trailingWhitespaceOnly = false;
                break;
            }
            const char ch = static_cast<char>(value);
            if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') {
                trailingWhitespaceOnly = false;
                break;
            }
        }
        file.close();
        if (error || !trailingWhitespaceOnly) {
            copy.status = JournalCopyStatus::Invalid;
            return;
        }
        copy.status = JournalCopyStatus::Parsed;
    };
    readOne(mirrors.primaryFs, mirrors.primary);
    readOne(mirrors.secondaryFs, mirrors.secondary);
    return true;
}

bool removeJournalAfterConvergence(StorageManager& storage, const char* path, const char* tempPath,
                                   JournalCopyStatus primaryStatus = JournalCopyStatus::Valid,
                                   JournalCopyStatus secondaryStatus = JournalCopyStatus::Valid) {
    if (!storage.isReady()) {
        return false;
    }
    StorageManager::SDLockBlocking lock(storage.getSDMutex());
    if (!lock) {
        return false;
    }
    fs::FS* primary = storage.getFilesystem();
    fs::FS* secondary = storage.getLittleFS();
    if (!primary) {
        return false;
    }
    if (secondary == primary) {
        secondary = nullptr;
    }
    const auto removeOne = [&](fs::FS* fs) {
        if (!fs) {
            return true;
        }
        if (tempPath && fs->exists(tempPath)) {
            fs->remove(tempPath);
        }
        return !fs->exists(path) || (fs->remove(path) && !fs->exists(path));
    };
    // Remove an invalid/missing mirror before the sole valid recovery record.
    // With two valid mirrors either may go first; a failed second removal still
    // leaves the other valid record for an idempotent retry.
    if (primaryStatus == JournalCopyStatus::Valid && secondaryStatus != JournalCopyStatus::Valid) {
        return removeOne(secondary) && removeOne(primary);
    }
    return removeOne(primary) && removeOne(secondary);
}

uint64_t allocateTransactionId(const char* sequenceKey, uint64_t selectedWatermark) {
    Preferences meta;
    if (!meta.begin(SETTINGS_NS_META, false)) {
        return 0;
    }
    const int64_t storedSequence = meta.getLong64(sequenceKey, 0);
    const uint64_t sequence = storedSequence > 0 ? static_cast<uint64_t>(storedSequence) : 0;
    const uint64_t floor = std::max(sequence, selectedWatermark);
    if (floor >= static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
        meta.end();
        return 0;
    }
    const uint64_t token = floor + 1u;
    const bool stored = meta.putLong64(sequenceKey, static_cast<int64_t>(token)) == sizeof(int64_t) &&
                        meta.getLong64(sequenceKey, 0) == static_cast<int64_t>(token);
    meta.end();
    return stored ? token : 0;
}

bool restoreMutatesCredentials(const JsonDocument& doc) {
    return doc["wifiStaSlots"].is<JsonArrayConst>() || legacyWifiClientSsidFromBackupDoc(doc).length() > 0 ||
           doc["wifiClientPasswordObf"].is<const char*>() || doc["stationPassword"].is<const char*>();
}

bool writeRestoreTransactionJournal(StorageManager& storage, uint64_t token, bool credentialsMutated,
                                    const RestoreCredentialSnapshot& credentialsBefore, bool profilesMutated,
                                    const std::vector<V1Profile>& profilesBefore) {
    JsonDocument doc;
    doc["_type"] = RESTORE_TRANSACTION_TYPE;
    doc["_version"] = RESTORE_TRANSACTION_VERSION;
    doc["token"] = token;
    doc["credentialsMutated"] = credentialsMutated;
    if (credentialsMutated) {
        writeCredentialSnapshotToJournal(doc["credentialsBefore"].to<JsonObject>(), credentialsBefore);
    }
    doc["profilesMutated"] = profilesMutated;
    if (profilesMutated) {
        JsonArray profiles = doc["profilesBefore"].to<JsonArray>();
        for (const V1Profile& profile : profilesBefore) {
            writeProfileToJournal(profiles.add<JsonObject>(), profile);
        }
    }
    stampJournalCrc(doc);
    return writeJournalAtomically(storage, RESTORE_TRANSACTION_PATH, RESTORE_TRANSACTION_TMP_PATH,
                                  RESTORE_TRANSACTION_MAX_BYTES, doc);
}

bool readRestoreTransactionJournal(const JsonDocument& doc, RestoreTransactionJournal& journal) {
    if (!doc["_type"].is<const char*>() || strcmp(doc["_type"].as<const char*>(), RESTORE_TRANSACTION_TYPE) != 0 ||
        !doc["_version"].is<int>() || doc["_version"].as<int>() != RESTORE_TRANSACTION_VERSION ||
        !doc["token"].is<int64_t>() || doc["token"].as<int64_t>() <= 0 ||
        !doc["credentialsMutated"].is<bool>() || !doc["profilesMutated"].is<bool>() ||
        !journalCrcValid(doc)) {
        return false;
    }
    journal.token = static_cast<uint64_t>(doc["token"].as<int64_t>());
    journal.credentialsMutated = doc["credentialsMutated"].as<bool>();
    if (journal.credentialsMutated &&
        (!doc["credentialsBefore"].is<JsonObjectConst>() ||
         !readCredentialSnapshotFromJournal(doc["credentialsBefore"].as<JsonObjectConst>(),
                                            journal.credentialsBefore))) {
        return false;
    }
    journal.profilesMutated = doc["profilesMutated"].as<bool>();
    if (journal.profilesMutated) {
        if (!doc["profilesBefore"].is<JsonArrayConst>()) {
            return false;
        }
        for (JsonVariantConst value : doc["profilesBefore"].as<JsonArrayConst>()) {
            if (!value.is<JsonObjectConst>()) {
                return false;
            }
            V1Profile profile;
            if (!readProfileFromJournal(value.as<JsonObjectConst>(), profile)) {
                return false;
            }
            journal.profilesBefore.push_back(profile);
        }
    }
    const size_t expectedKeys = 6 + (journal.credentialsMutated ? 1 : 0) +
                                (journal.profilesMutated ? 1 : 0);
    return jsonHasExactSize(doc.as<JsonObjectConst>(), expectedKeys);
}

bool writeProfileDeleteTransactionJournal(StorageManager& storage, uint64_t token, const V1Profile& profile,
                                          bool hadReferences) {
    JsonDocument doc;
    doc["_type"] = PROFILE_DELETE_TRANSACTION_TYPE;
    doc["_version"] = PROFILE_DELETE_TRANSACTION_VERSION;
    doc["token"] = static_cast<int64_t>(token);
    doc["hadReferences"] = hadReferences;
    writeProfileToJournal(doc["profile"].to<JsonObject>(), profile);
    stampJournalCrc(doc);
    return writeJournalAtomically(storage, PROFILE_DELETE_TRANSACTION_PATH,
                                  PROFILE_DELETE_TRANSACTION_TMP_PATH,
                                  PROFILE_DELETE_TRANSACTION_MAX_BYTES, doc);
}

bool readProfileDeleteTransactionJournal(const JsonDocument& doc, ProfileDeleteTransactionJournal& journal) {
    if (!doc["_type"].is<const char*>() ||
        strcmp(doc["_type"].as<const char*>(), PROFILE_DELETE_TRANSACTION_TYPE) != 0 ||
        !doc["_version"].is<int>() || doc["_version"].as<int>() != PROFILE_DELETE_TRANSACTION_VERSION ||
        !doc["token"].is<int64_t>() || doc["token"].as<int64_t>() <= 0 ||
        !doc["hadReferences"].is<bool>() || !doc["profile"].is<JsonObjectConst>() ||
        !journalCrcValid(doc) || !jsonHasExactSize(doc.as<JsonObjectConst>(), 6)) {
        return false;
    }
    journal.token = static_cast<uint64_t>(doc["token"].as<int64_t>());
    journal.hadReferences = doc["hadReferences"].as<bool>();
    return readProfileFromJournal(doc["profile"].as<JsonObjectConst>(), journal.profile);
}

struct MirroredJournalMessages {
    const char* invalidCopies;
    const char* ambiguousPendingCopies;
    const char* committedOrObsolete;
    const char* cleanupRetry;
};

template <typename Journal, typename ReadJournal, typename JournalsEqual, typename RecoverPending>
bool resolveMirroredTransactionJournal(StorageManager& storage, const char* path, const char* tempPath,
                                       size_t maxBytes, uint64_t commitWatermark,
                                       const MirroredJournalMessages& messages, ReadJournal readJournal,
                                       JournalsEqual journalsEqual, RecoverPending recoverPending) {
    JournalMirrors mirrors;
    if (!loadJournalMirrors(storage, path, maxBytes, mirrors)) {
        return false;
    }
    if (mirrors.primary.status == JournalCopyStatus::Missing &&
        mirrors.secondary.status == JournalCopyStatus::Missing) {
        return true;
    }

    Journal primaryJournal;
    Journal secondaryJournal;
    const auto validateCopy = [&](JournalCopy& copy, Journal& journal) {
        if (copy.status == JournalCopyStatus::Parsed) {
            copy.status = readJournal(copy.doc, journal) ? JournalCopyStatus::Valid
                                                        : JournalCopyStatus::Invalid;
        }
    };
    validateCopy(mirrors.primary, primaryJournal);
    validateCopy(mirrors.secondary, secondaryJournal);

    const bool primaryValid = mirrors.primary.status == JournalCopyStatus::Valid;
    const bool secondaryValid = mirrors.secondary.status == JournalCopyStatus::Valid;
    if (!primaryValid && !secondaryValid) {
        Serial.println(messages.invalidCopies);
        return false;
    }

    const Journal* journal = primaryValid ? &primaryJournal : &secondaryJournal;
    if (primaryValid && secondaryValid && !journalsEqual(primaryJournal, secondaryJournal)) {
        const bool primaryCommitted = primaryJournal.token <= commitWatermark;
        const bool secondaryCommitted = secondaryJournal.token <= commitWatermark;
        if (primaryCommitted && secondaryCommitted) {
            journal = nullptr;
        } else if (primaryCommitted != secondaryCommitted) {
            journal = primaryCommitted ? &secondaryJournal : &primaryJournal;
        } else {
            Serial.println(messages.ambiguousPendingCopies);
            return false;
        }
    }

    // Recovery is rollback-only. At or below the durable watermark the
    // transaction already committed, and its payload may name data recreated
    // later (notably a same-name profile after deletion).
    if (journal && journal->token > commitWatermark) {
        if (!recoverPending(*journal)) {
            return false;
        }
    } else {
        Serial.println(messages.committedOrObsolete);
    }

    if (!removeJournalAfterConvergence(storage, path, tempPath, mirrors.primary.status,
                                       mirrors.secondary.status)) {
        if (messages.cleanupRetry) {
            Serial.println(messages.cleanupRetry);
        }
        return false;
    }
    return true;
}

ProfileOperationResult profileOperationResult(ProfileStorageStatus status, const String& error = "") {
    ProfileOperationResult result;
    result.status = status;
    result.error = error;
    return result;
}

bool captureRestoreCredentialSnapshot(StorageManager& storage, RestoreCredentialSnapshot& snapshot) {
    Preferences prefs;
    if (!prefs.begin(WIFI_CLIENT_NS, true)) {
        return false;
    }
    for (size_t i = 0; i < kWifiStaSlotCount; ++i) {
        snapshot.passwordPresent[i] = prefs.isKey(kNvsWifiStaSlotPassword[i]);
        if (snapshot.passwordPresent[i]) {
            snapshot.passwordValues[i] = prefs.getString(kNvsWifiStaSlotPassword[i], "");
        }
    }
    snapshot.legacyPasswordPresent = prefs.isKey(kNvsWifiPassword);
    if (snapshot.legacyPasswordPresent) {
        snapshot.legacyPasswordValue = prefs.getString(kNvsWifiPassword, "");
    }
    prefs.end();

    snapshot.sdRelevant = storage.isReady() && storage.isSDCard();
    if (!snapshot.sdRelevant) {
        return true;
    }
    StorageManager::SDLockBlocking lock(storage.getSDMutex());
    if (!lock) {
        return false;
    }
    fs::FS* fs = storage.getFilesystem();
    if (!fs) {
        return false;
    }
    snapshot.sdFilePresent = fs->exists(WIFI_CLIENT_SD_SECRET_PATH);
    if (!snapshot.sdFilePresent) {
        return true;
    }
    File file = fs->open(WIFI_CLIENT_SD_SECRET_PATH, FILE_READ);
    if (!file) {
        return false;
    }
    const size_t expected = file.size();
    snapshot.sdFileBytes.reserve(expected);
    while (file.available() > 0) {
        const int value = file.read();
        if (value < 0) {
            break;
        }
        snapshot.sdFileBytes += static_cast<char>(value);
    }
    file.close();
    return snapshot.sdFileBytes.length() == expected;
}

bool restoreCredentialSnapshot(StorageManager& storage, const RestoreCredentialSnapshot& snapshot) {
    Preferences prefs;
    if (!prefs.begin(WIFI_CLIENT_NS, false)) {
        return false;
    }
    bool restored = true;
    const auto restoreKey = [&](const char* key, bool present, const String& value) {
        if (!present) {
            if (prefs.isKey(key)) {
                restored = prefs.remove(key) && restored;
            }
            restored = !prefs.isKey(key) && restored;
            return;
        }
        const size_t written = prefs.putString(key, value);
        restored = written == value.length() && prefs.isKey(key) && prefs.getString(key, "") == value && restored;
    };
    for (size_t i = 0; i < kWifiStaSlotCount; ++i) {
        restoreKey(kNvsWifiStaSlotPassword[i], snapshot.passwordPresent[i], snapshot.passwordValues[i]);
    }
    restoreKey(kNvsWifiPassword, snapshot.legacyPasswordPresent, snapshot.legacyPasswordValue);
    prefs.end();
    if (!snapshot.sdRelevant) {
        return restored;
    }

    StorageManager::SDLockBlocking lock(storage.getSDMutex());
    if (!lock) {
        return false;
    }
    fs::FS* fs = storage.getFilesystem();
    if (!fs) {
        return false;
    }
    constexpr const char* tempPath = "/v1wifi_secret.restore.tmp";
    if (!snapshot.sdFilePresent) {
        if (fs->exists(WIFI_CLIENT_SD_SECRET_PATH)) {
            const String rollbackPath = StorageManager::rollbackPathFor(WIFI_CLIENT_SD_SECRET_PATH);
            if (fs->exists(rollbackPath.c_str())) {
                fs->remove(rollbackPath.c_str());
            }
            restored = fs->rename(WIFI_CLIENT_SD_SECRET_PATH, rollbackPath.c_str()) && restored;
            fs->remove(rollbackPath.c_str());
        }
        return restored;
    }
    if (fs->exists(tempPath)) {
        fs->remove(tempPath);
    }
    File file = fs->open(tempPath, FILE_WRITE);
    if (!file) {
        return false;
    }
    const size_t written = file.write(reinterpret_cast<const uint8_t*>(snapshot.sdFileBytes.c_str()),
                                      snapshot.sdFileBytes.length());
    file.flush();
    file.close();
    if (written != snapshot.sdFileBytes.length()) {
        fs->remove(tempPath);
        return false;
    }
    return StorageManager::promoteTempFileWithRollback(*fs, tempPath, WIFI_CLIENT_SD_SECRET_PATH) && restored;
}

} // namespace

bool backupDocumentCanApply(const JsonDocument& doc, const V1Settings& current, V1ProfileManager& profiles) {
    std::vector<V1Profile> incomingProfiles;
    std::vector<V1Profile> existingProfiles;
    return validateBackupDocumentForApply(doc, current, profiles, incomingProfiles, existingProfiles);
}

bool SettingsManager::resolveRestoreTransaction() {
    const MirroredJournalMessages messages = {
        "[Settings] ERROR: Restore journals exist but neither mirror is valid",
        "[Settings] ERROR: Divergent pending restore journals are ambiguous",
        "[Settings] Recovered committed/obsolete restore transaction",
        "[Settings] WARN: Restore journal cleanup will retry",
    };
    return resolveMirroredTransactionJournal<RestoreTransactionJournal>(
        *storage_, RESTORE_TRANSACTION_PATH, RESTORE_TRANSACTION_TMP_PATH,
        RESTORE_TRANSACTION_MAX_BYTES, restoreCommitWatermark_, messages,
        readRestoreTransactionJournal, restoreJournalsEqual,
        [this](const RestoreTransactionJournal& journal) {
            bool recovered = true;
            if (journal.credentialsMutated) {
                recovered = restoreCredentialSnapshot(*storage_, journal.credentialsBefore) && recovered;
            }
            if (journal.profilesMutated) {
                if (!profiles_->isReady()) {
                    Serial.println("[Settings] Restore transaction recovery waiting for profile storage");
                    return false;
                }
                recovered = restoreProfileSnapshot(*profiles_, journal.profilesBefore) && recovered;
            }
            if (!recovered) {
                Serial.println("[Settings] ERROR: Pending restore transaction rollback incomplete");
                return false;
            }
            Serial.println("[Settings] Rolled back interrupted restore transaction");
            return true;
        });
}

bool SettingsManager::resolveProfileDeleteTransaction() {
    const MirroredJournalMessages messages = {
        "[Settings] ERROR: Profile-delete journals exist but neither mirror is valid",
        "[Settings] ERROR: Divergent pending profile-delete journals are ambiguous",
        "[Settings] Recovered committed/obsolete profile delete",
        nullptr,
    };
    return resolveMirroredTransactionJournal<ProfileDeleteTransactionJournal>(
        *storage_, PROFILE_DELETE_TRANSACTION_PATH, PROFILE_DELETE_TRANSACTION_TMP_PATH,
        PROFILE_DELETE_TRANSACTION_MAX_BYTES, profileDeleteCommitWatermark_, messages,
        readProfileDeleteTransactionJournal, deleteJournalsEqual,
        [this](const ProfileDeleteTransactionJournal& journal) {
            if (!profiles_->isReady()) {
                Serial.println("[Settings] Profile delete recovery waiting for profile storage");
                return false;
            }
            if (!profiles_->saveProfile(journal.profile).success) {
                Serial.printf("[Settings] ERROR: Could not restore profile '%s' after interrupted delete\n",
                              journal.profile.name.c_str());
                return false;
            }
            Serial.printf("[Settings] Restored profile '%s' after interrupted delete\n",
                          journal.profile.name.c_str());
            return true;
        });
}

bool SettingsManager::resolveStorageTransactionsForMutation() {
    if (!resolveWifiCredentialTransaction()) {
        return false;
    }
    if (!resolveRestoreTransaction()) {
        return false;
    }
    return resolveProfileDeleteTransaction();
}

ProfileOperationResult SettingsManager::deleteProfileAndReferences(const String& canonicalProfileName) {
    if (!resolveRestoreTransaction() || !resolveProfileDeleteTransaction()) {
        return profileOperationResult(ProfileStorageStatus::IoError, "Storage recovery pending");
    }

    V1Profile profile;
    const ProfileOperationResult loaded = profiles_->loadProfileResult(canonicalProfileName, profile, 0);
    if (!loaded.success()) {
        return loaded;
    }
    const bool hadReferences = settings_.slot0_default.profileName == profile.name ||
                               settings_.slot1_highway.profileName == profile.name ||
                               settings_.slot2_comfort.profileName == profile.name;
    const uint64_t token = allocateTransactionId(kNvsProfileDeleteTransactionSequence,
                                                 profileDeleteCommitWatermark_);
    if (token == 0 || !writeProfileDeleteTransactionJournal(*storage_, token, profile, hadReferences)) {
        return profileOperationResult(ProfileStorageStatus::IoError, "Failed to prepare profile delete");
    }

#ifdef UNIT_TEST
    if (profileDeleteInterruptAfterJournal_) {
        profileDeleteInterruptAfterJournal_ = false;
        return profileOperationResult(ProfileStorageStatus::IoError,
                                      "Simulated reset after profile delete journal");
    }
#endif

    const ProfileOperationResult deleted = profiles_->deleteProfileResult(profile.name, 250);
    if (!deleted.success()) {
        if (deleted.status == ProfileStorageStatus::Busy) {
            if (!removeJournalAfterConvergence(*storage_, PROFILE_DELETE_TRANSACTION_PATH,
                                               PROFILE_DELETE_TRANSACTION_TMP_PATH)) {
                return profileOperationResult(ProfileStorageStatus::IoError,
                                              "Failed to cancel busy profile delete");
            }
            return deleted;
        }
        // An I/O failure can occur after part of the profile transaction was
        // attempted. Persisted assignments are still old, so recovery restores
        // the snapshotted profile before reporting the failure when possible.
        resolveProfileDeleteTransaction();
        return deleted;
    }

#ifdef UNIT_TEST
    if (profileDeleteInterruptAfterProfile_) {
        profileDeleteInterruptAfterProfile_ = false;
        return profileOperationResult(ProfileStorageStatus::IoError,
                                      "Simulated reset after profile delete");
    }
#endif

    const V1Settings settingsBeforeDelete = settings_;
    const uint64_t deleteWatermarkBefore = profileDeleteCommitWatermark_;
    for (int slotIndex = 0; slotIndex < 3; ++slotIndex) {
        V1Settings::AutoPushSlotView slot = settings_.autoPushSlotView(slotIndex);
        if (slot.config.profileName == profile.name) {
            slot.config.profileName = "";
        }
    }
    profileDeleteCommitWatermark_ = token;
    // The authority commit is required even when the profile was unassigned;
    // otherwise a stale delete journal could later target a same-name profile.
    if (!saveDeferredBackup()) {
        settings_ = settingsBeforeDelete;
        profileDeleteCommitWatermark_ = deleteWatermarkBefore;
        clearDeferredPersistState();
        // The selected settings copy still contains the old assignments.
        // Recovery therefore restores the profile. If that compensation also
        // fails, the journal remains for the next boot to retry.
        resolveProfileDeleteTransaction();
        return profileOperationResult(ProfileStorageStatus::IoError,
                                      "Failed to persist cleared profile assignments");
    }


#ifdef UNIT_TEST
    if (profileDeleteInterruptAfterReferences_) {
        profileDeleteInterruptAfterReferences_ = false;
        return profileOperationResult(ProfileStorageStatus::IoError,
                                      "Simulated reset after profile assignment commit");
    }
#endif

    // At this point profile deletion and assignment clearing are both durable.
    // A leftover journal is harmless: boot recovery observes no references and
    // completes the same deletion before removing it.
    bool leaveJournal = false;
#ifdef UNIT_TEST
    leaveJournal = leaveProfileDeleteJournalAfterCommit_;
    leaveProfileDeleteJournalAfterCommit_ = false;
#endif
    if (!leaveJournal && !removeJournalAfterConvergence(*storage_, PROFILE_DELETE_TRANSACTION_PATH,
                                                        PROFILE_DELETE_TRANSACTION_TMP_PATH)) {
        Serial.println("[Settings] WARN: Committed profile delete journal cleanup will retry");
    }
    return profileOperationResult(ProfileStorageStatus::Success);
}

SettingsBackupApplyResult SettingsManager::applyBackupDocument(const JsonDocument& doc, bool deferBackupRewrite,
                                                               const SettingsRestoreWatchdog& watchdog) {
    SettingsBackupApplyResult result;
    // Fed at restore phase boundaries only — see SettingsRestoreWatchdog.
    auto feedWatchdog = [&watchdog]() {
        if (watchdog.feed) {
            watchdog.feed(watchdog.ctx);
        }
    };

    if (!resolveWifiCredentialTransaction()) {
        Serial.println("[Settings] ERROR: Cannot restore while WiFi credential recovery is pending");
        return result;
    }
    if (!resolveRestoreTransaction() || !resolveProfileDeleteTransaction()) {
        Serial.println("[Settings] ERROR: Cannot restore while storage recovery is pending");
        return result;
    }

    const V1Settings settingsBefore = settings_;
    const bool restorePendingBefore = restorePending_;
    const uint64_t restoreWatermarkBefore = restoreCommitWatermark_;
    std::vector<V1Profile> incomingProfiles;
    std::vector<V1Profile> profilesBefore;
    if (!validateBackupDocumentForApply(doc, settingsBefore, *profiles_, incomingProfiles, profilesBefore)) {
        Serial.println("[Settings] ERROR: Backup document failed transaction validation");
        return result;
    }
    RestoreCredentialSnapshot credentialsBefore;
    if (!captureRestoreCredentialSnapshot(*storage_, credentialsBefore)) {
        Serial.println("[Settings] ERROR: Failed to snapshot credential stores before restore");
        return result;
    }

    const bool credentialsMutated = restoreMutatesCredentials(doc);
    const bool profilesMutated = !incomingProfiles.empty();
    const bool externalStoresMutated = credentialsMutated || profilesMutated;
    bool journalWritten = false;
    uint64_t restoreToken = 0;
    if (externalStoresMutated) {
        restoreToken = allocateTransactionId(kNvsRestoreTransactionSequence,
                                             restoreCommitWatermark_);
        if (restoreToken == 0 ||
            !writeRestoreTransactionJournal(*storage_, restoreToken, credentialsMutated, credentialsBefore,
                                            profilesMutated, profilesBefore)) {
            Serial.println("[Settings] ERROR: Failed to prepare durable restore transaction");
            return result;
        }
        journalWritten = true;
    }

    const auto rollback = [&]() {
        settings_ = settingsBefore;
        restorePending_ = restorePendingBefore;
        restoreCommitWatermark_ = restoreWatermarkBefore;
        const bool credentialsRestored = !credentialsMutated || restoreCredentialSnapshot(*storage_, credentialsBefore);
        const bool profilesRestored = !profilesMutated || restoreProfileSnapshot(*profiles_, profilesBefore);
        if (!credentialsRestored || !profilesRestored) {
            Serial.printf("[Settings] ERROR: Restore rollback incomplete credentials=%s profiles=%s\n",
                          credentialsRestored ? "ok" : "failed", profilesRestored ? "ok" : "failed");
        }
        if (journalWritten && credentialsRestored && profilesRestored &&
            !removeJournalAfterConvergence(*storage_, RESTORE_TRANSACTION_PATH,
                                           RESTORE_TRANSACTION_TMP_PATH)) {
            Serial.println("[Settings] WARN: Restore rollback journal cleanup will retry on boot");
        }
    };

    if (!applyBackupNetworkFields(doc, settings_, *storage_, BackupRestoreScope::Full, deferBackupRewrite)) {
        Serial.println("[Settings] ERROR: Network credential restore failed; rolling back document");
        rollback();
        return result;
    }
    feedWatchdog();

#ifdef UNIT_TEST
    if (restoreInterruptAfterCredentials_) {
        restoreInterruptAfterCredentials_ = false;
        Serial.println("[Settings] TEST: Simulating reset after restore credential writes");
        return result;
    }
#endif

    applyBackupDisplayFields(doc, settings_, BackupRestoreScope::Full);
    applyBackupAudioFields(doc, settings_, BackupRestoreScope::Full);
    applyBackupProfileSlotFields(doc, settings_, BackupRestoreScope::Full);
    applyBackupObdFields(doc, settings_, BackupRestoreScope::Full);
    applyBackupAlpAndGpsFields(doc, settings_);
    healBackupRestoreConflicts(settings_, "restored");
    feedWatchdog();

    int profilesProcessed = 0;
    for (const V1Profile& profile : incomingProfiles) {
        if (++profilesProcessed % kProfileRestoreWatchdogFeedInterval == 0) {
            feedWatchdog();
        }
        const ProfileSaveResult saveResult = profiles_->saveProfile(profile);
        if (!saveResult.success) {
            Serial.println("[Settings] ERROR: Profile restore failed; rolling back document");
            rollback();
            return result;
        }
    }

#ifdef UNIT_TEST
    if (restoreInterruptAfterProfiles_) {
        restoreInterruptAfterProfiles_ = false;
        Serial.println("[Settings] TEST: Simulating reset after restore profile writes");
        return result;
    }
#endif

    const bool wasRestorePending = restorePending_;
    clearRestorePending();
    if (journalWritten) {
        // This value becomes authoritative only when the A/B settings selector
        // advances. Until then the filesystem journal remains rollback intent.
        restoreCommitWatermark_ = restoreToken;
    }

    // Phase 4 done: profile writes are finished and the A/B NVS rewrite below is
    // about to start with a full watchdog window in front of it.  The rewrite
    // itself lives in persistSettingsAtomically()/saveDeferredBackup() and is
    // not instrumented here.
    feedWatchdog();

    if (deferBackupRewrite) {
        if (!saveDeferredBackup()) {
            restorePending_ = wasRestorePending;
            Serial.println("[Settings] ERROR: Failed to persist restored settings_");
            rollback();
            return result;
        }
    } else {
        if (!persistSettingsAtomically()) {
            restorePending_ = wasRestorePending;
            Serial.println("[Settings] ERROR: Failed to persist restored settings_");
            rollback();
            return result;
        }
        noteNvsCommitWithoutBackupIntent();
    }

    // Phase 5 done: persist finished; hand the caller a fresh window to build
    // and send its response on.
    feedWatchdog();

    if (journalWritten) {
        bool leaveJournal = false;
#ifdef UNIT_TEST
        leaveJournal = leaveRestoreJournalAfterCommit_;
        leaveRestoreJournalAfterCommit_ = false;
#endif
        if (!leaveJournal &&
            !removeJournalAfterConvergence(*storage_, RESTORE_TRANSACTION_PATH,
                                           RESTORE_TRANSACTION_TMP_PATH)) {
            Serial.println("[Settings] WARN: Committed restore journal cleanup will retry on boot");
        }
    }

    result.success = true;
    result.profilesRestored = static_cast<int>(incomingProfiles.size());
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
