/**
 * Settings NVS persistence layer, crypto/obfuscation, and WiFi client credentials.
 * Extracted from settings_.cpp to reduce file size.
 */

#include "settings_internals.h"

// --- NVS recovery, crypto, WiFi SD secret helpers ---

namespace {

constexpr const char* WIFI_CLIENT_BACKUP_PASSWORD_KEY = "wifiClientPasswordObf";
constexpr const char* WIFI_STA_SLOT_BACKUP_PASSWORD_KEY = "passwordObf";
constexpr const char* WIFI_CLIENT_SD_SECRETS_KEY = "secrets";
constexpr const char* WIFI_CLIENT_SD_SECRET_INDEX_KEY = "index";
constexpr const char* WIFI_CLIENT_SD_SECRET_SSID_KEY = "ssid";
constexpr const char* WIFI_CLIENT_SD_SECRET_PASSWORD_KEY = "password_obf";
constexpr const char* WIFI_CLIENT_SD_SECRET_TIMESTAMP_KEY = "timestamp";

struct WifiClientSdSecretEntry {
    bool used = false;
    String ssid;
    String encodedPassword;
    uint32_t timestamp = 0;
};

bool validWifiStaSlotIndex(size_t index) {
    return index < kWifiStaSlotCount;
}

const char* wifiStaSlotPasswordKey(size_t index) {
    return validWifiStaSlotIndex(index) ? kNvsWifiStaSlotPassword[index] : nullptr;
}

String wifiClientPasswordObfFromBackupDoc(const JsonDocument& doc, const String& expectedSsid) {
    if (doc["wifiStaSlots"].is<JsonArrayConst>()) {
        JsonArrayConst slots = doc["wifiStaSlots"].as<JsonArrayConst>();
        for (JsonObjectConst slotObj : slots) {
            if (!slotObj["ssid"].is<const char*>()) {
                continue;
            }
            const String slotSsid = sanitizeWifiClientSsidValue(slotObj["ssid"].as<String>());
            if (expectedSsid.length() > 0 && slotSsid.length() > 0 && slotSsid != expectedSsid) {
                continue;
            }
            if (!slotObj[WIFI_STA_SLOT_BACKUP_PASSWORD_KEY].is<const char*>()) {
                continue;
            }
            const String encoded = slotObj[WIFI_STA_SLOT_BACKUP_PASSWORD_KEY].as<String>();
            if (encoded.length() > 0 && decodeObfuscatedFromStorage(encoded).length() > 0) {
                return encoded;
            }
        }
    }

    if (!doc[WIFI_CLIENT_BACKUP_PASSWORD_KEY].is<const char*>()) {
        return "";
    }

    const String backupSsid = doc["wifiClientSSID"] | "";
    if (expectedSsid.length() > 0 && backupSsid.length() > 0 && backupSsid != expectedSsid) {
        return "";
    }

    const String encoded = doc[WIFI_CLIENT_BACKUP_PASSWORD_KEY].as<String>();
    if (encoded.length() == 0) {
        return "";
    }

    // Main-backup credentials are only written for non-empty passwords.  Empty
    // decode means corruption or an unsupported encoding, not an open network.
    return decodeObfuscatedFromStorage(encoded).length() > 0 ? encoded : "";
}

bool wifiClientSdSecretTypeMatches(const JsonDocument& doc) {
    const char* type = doc["_type"] | "";
    return strcmp(type, WIFI_CLIENT_SD_SECRET_TYPE) == 0;
}

String ssidFromWifiClientSdSecret(JsonObjectConst entry) {
    if (!entry[WIFI_CLIENT_SD_SECRET_SSID_KEY].is<const char*>()) {
        return "";
    }
    return sanitizeWifiClientSsidValue(entry[WIFI_CLIENT_SD_SECRET_SSID_KEY].as<String>());
}

String passwordFromWifiClientSdSecret(JsonObjectConst entry) {
    if (!entry[WIFI_CLIENT_SD_SECRET_PASSWORD_KEY].is<const char*>()) {
        return "";
    }
    return entry[WIFI_CLIENT_SD_SECRET_PASSWORD_KEY].as<String>();
}

bool wifiClientSdSecretMatches(JsonObjectConst entry, const String& expectedSsid, size_t expectedSlotIndex,
                               bool requireSlotMatch) {
    const String savedSsid = ssidFromWifiClientSdSecret(entry);
    if (savedSsid.length() == 0) {
        return false;
    }
    if (expectedSsid.length() > 0 && savedSsid != expectedSsid) {
        return false;
    }
    if (!requireSlotMatch) {
        return true;
    }
    if (!validWifiStaSlotIndex(expectedSlotIndex) || !entry[WIFI_CLIENT_SD_SECRET_INDEX_KEY].is<int>()) {
        return false;
    }
    return entry[WIFI_CLIENT_SD_SECRET_INDEX_KEY].as<int>() == static_cast<int>(expectedSlotIndex);
}

String findWifiClientSdSecretInArray(const JsonDocument& doc, const String& expectedSsid, size_t expectedSlotIndex,
                                     bool requireSlotMatch) {
    if (!doc[WIFI_CLIENT_SD_SECRETS_KEY].is<JsonArrayConst>()) {
        return "";
    }

    JsonArrayConst secrets = doc[WIFI_CLIENT_SD_SECRETS_KEY].as<JsonArrayConst>();
    for (JsonObjectConst entry : secrets) {
        if (!wifiClientSdSecretMatches(entry, expectedSsid, expectedSlotIndex, requireSlotMatch)) {
            continue;
        }
        return passwordFromWifiClientSdSecret(entry);
    }
    return "";
}

bool readWifiClientSdSecretEntries(const JsonDocument& doc, WifiClientSdSecretEntry entries[kWifiStaSlotCount]) {
    if (!wifiClientSdSecretTypeMatches(doc)) {
        return false;
    }

    bool foundAny = false;
    if (doc[WIFI_CLIENT_SD_SECRETS_KEY].is<JsonArrayConst>()) {
        JsonArrayConst secrets = doc[WIFI_CLIENT_SD_SECRETS_KEY].as<JsonArrayConst>();
        for (JsonObjectConst entry : secrets) {
            if (!entry[WIFI_CLIENT_SD_SECRET_INDEX_KEY].is<int>()) {
                continue;
            }
            const int rawIndex = entry[WIFI_CLIENT_SD_SECRET_INDEX_KEY].as<int>();
            if (rawIndex < 0 || rawIndex >= static_cast<int>(kWifiStaSlotCount)) {
                continue;
            }

            const String ssid = ssidFromWifiClientSdSecret(entry);
            if (ssid.length() == 0) {
                continue;
            }

            WifiClientSdSecretEntry& target = entries[static_cast<size_t>(rawIndex)];
            target.used = true;
            target.ssid = ssid;
            target.encodedPassword = passwordFromWifiClientSdSecret(entry);
            if (entry[WIFI_CLIENT_SD_SECRET_TIMESTAMP_KEY].is<uint32_t>()) {
                target.timestamp = entry[WIFI_CLIENT_SD_SECRET_TIMESTAMP_KEY].as<uint32_t>();
            } else if (entry[WIFI_CLIENT_SD_SECRET_TIMESTAMP_KEY].is<int>()) {
                target.timestamp =
                    static_cast<uint32_t>(std::max(0, entry[WIFI_CLIENT_SD_SECRET_TIMESTAMP_KEY].as<int>()));
            }
            foundAny = true;
        }
    }

    // Legacy v1 file shape had one top-level SSID/password pair.  Preserve it
    // under the first free slot so upgraded firmware can merge another saved
    // network without discarding the only old recovery copy.
    const String legacySsid = sanitizeWifiClientSsidValue(doc[WIFI_CLIENT_SD_SECRET_SSID_KEY] | "");
    if (legacySsid.length() > 0) {
        bool alreadyPresent = false;
        for (size_t i = 0; i < kWifiStaSlotCount; ++i) {
            if (entries[i].used && entries[i].ssid == legacySsid) {
                alreadyPresent = true;
                break;
            }
        }
        if (!alreadyPresent) {
            for (size_t i = 0; i < kWifiStaSlotCount; ++i) {
                if (entries[i].used) {
                    continue;
                }
                entries[i].used = true;
                entries[i].ssid = legacySsid;
                entries[i].encodedPassword = doc[WIFI_CLIENT_SD_SECRET_PASSWORD_KEY] | "";
                if (doc[WIFI_CLIENT_SD_SECRET_TIMESTAMP_KEY].is<uint32_t>()) {
                    entries[i].timestamp = doc[WIFI_CLIENT_SD_SECRET_TIMESTAMP_KEY].as<uint32_t>();
                } else if (doc[WIFI_CLIENT_SD_SECRET_TIMESTAMP_KEY].is<int>()) {
                    entries[i].timestamp =
                        static_cast<uint32_t>(std::max(0, doc[WIFI_CLIENT_SD_SECRET_TIMESTAMP_KEY].as<int>()));
                }
                foundAny = true;
                break;
            }
        }
    }

    return foundAny;
}

bool loadWifiClientSdSecretDocument(fs::FS* fs, JsonDocument& doc) {
    if (!fs || !fs->exists(WIFI_CLIENT_SD_SECRET_PATH)) {
        return false;
    }

    File file = fs->open(WIFI_CLIENT_SD_SECRET_PATH, FILE_READ);
    if (!file) {
        return false;
    }

    DeserializationError err = deserializeJson(doc, file);
    file.close();
    if (err) {
        Serial.printf("[Settings] WARN: Failed to parse SD WiFi secret: %s\n", err.c_str());
        return false;
    }

    return wifiClientSdSecretTypeMatches(doc);
}

bool writeWifiClientSdSecretEntries(fs::FS* fs, const WifiClientSdSecretEntry entries[kWifiStaSlotCount],
                                    size_t preferredLegacyIndex) {
    if (!fs) {
        return false;
    }

    size_t legacyIndex = kWifiStaSlotCount;
    if (validWifiStaSlotIndex(preferredLegacyIndex) && entries[preferredLegacyIndex].used) {
        legacyIndex = preferredLegacyIndex;
    } else {
        for (size_t i = 0; i < kWifiStaSlotCount; ++i) {
            if (entries[i].used) {
                legacyIndex = i;
                break;
            }
        }
    }

    if (!validWifiStaSlotIndex(legacyIndex)) {
        if (fs->exists(WIFI_CLIENT_SD_SECRET_PATH)) {
            fs->remove(WIFI_CLIENT_SD_SECRET_PATH);
        }
        return true;
    }

    JsonDocument doc;
    doc["_type"] = WIFI_CLIENT_SD_SECRET_TYPE;
    doc["_version"] = WIFI_CLIENT_SD_SECRET_VERSION;
    JsonArray secrets = doc[WIFI_CLIENT_SD_SECRETS_KEY].to<JsonArray>();
    for (size_t i = 0; i < kWifiStaSlotCount; ++i) {
        if (!entries[i].used || entries[i].ssid.length() == 0) {
            continue;
        }

        JsonObject entry = secrets.add<JsonObject>();
        entry[WIFI_CLIENT_SD_SECRET_INDEX_KEY] = static_cast<uint8_t>(i);
        entry[WIFI_CLIENT_SD_SECRET_SSID_KEY] = entries[i].ssid;
        entry[WIFI_CLIENT_SD_SECRET_PASSWORD_KEY] = entries[i].encodedPassword;
        entry[WIFI_CLIENT_SD_SECRET_TIMESTAMP_KEY] = entries[i].timestamp;
    }

    // Keep v1 top-level fields populated so older firmware can still recover
    // the most recently touched network, even though v2 stores all slots.
    doc[WIFI_CLIENT_SD_SECRET_SSID_KEY] = entries[legacyIndex].ssid;
    doc[WIFI_CLIENT_SD_SECRET_PASSWORD_KEY] = entries[legacyIndex].encodedPassword;
    doc[WIFI_CLIENT_SD_SECRET_TIMESTAMP_KEY] = entries[legacyIndex].timestamp;

    File file = fs->open(WIFI_CLIENT_SD_SECRET_PATH, FILE_WRITE);
    if (!file) {
        Serial.println("[Settings] WARN: Failed to open SD WiFi secret file for write");
        return false;
    }

    serializeJson(doc, file);
    file.flush();
    file.close();
    return true;
}

String loadWifiClientPasswordObfFromSettingsBackup(fs::FS* fs, const String& expectedSsid) {
    if (!fs) {
        return "";
    }

    JsonDocument backupDoc;
    const char* backupPath = nullptr;
    if (!loadBestBackupDocument(fs, backupDoc, &backupPath, false)) {
        return "";
    }

    return wifiClientPasswordObfFromBackupDoc(backupDoc, expectedSsid);
}

} // namespace

// NVS recovery: clear unused namespace when NVS is full
// Returns true if space was freed
bool attemptNvsRecovery(const char* activeNs) {
    Serial.println("[Settings] NVS space low - attempting recovery...");

    // Clear the inactive settings namespace to free space
    const char* inactiveNs = nullptr;
    if (strcmp(activeNs, SETTINGS_NS_A) == 0) {
        inactiveNs = SETTINGS_NS_B;
    } else if (strcmp(activeNs, SETTINGS_NS_B) == 0) {
        inactiveNs = SETTINGS_NS_A;
    }

    bool recovered = false;
    if (inactiveNs) {
        Preferences prefs;
        if (prefs.begin(inactiveNs, false)) {
            prefs.clear();
            prefs.end();
            Serial.printf("[Settings] Cleared inactive namespace %s\n", inactiveNs);
            recovered = true;
        }
    }

    return recovered;
}

// xorObfuscate, hexDigit/hexNibble, bytesToHex/hexToBytes,
// encodeObfuscatedForStorage, decodeObfuscatedFromStorage
// are defined in settings_backup.cpp.

bool saveWifiClientSecretToSD(size_t slotIndex, const String& ssid, const String& encodedPassword) {
    if (!storageManager.isReady() || !storageManager.isSDCard()) {
        return false;
    }

    StorageManager::SDLockBlocking sdLock(storageManager.getSDMutex());
    if (!sdLock) {
        Serial.println("[Settings] WARN: Failed to acquire SD mutex for WiFi secret save");
        return false;
    }

    fs::FS* fs = storageManager.getFilesystem();
    if (!fs) {
        return false;
    }

    WifiClientSdSecretEntry entries[kWifiStaSlotCount];
    JsonDocument existingDoc;
    if (loadWifiClientSdSecretDocument(fs, existingDoc)) {
        readWifiClientSdSecretEntries(existingDoc, entries);
    }

    const String sanitizedSsid = sanitizeWifiClientSsidValue(ssid);
    if (!existingDoc[WIFI_CLIENT_SD_SECRETS_KEY].is<JsonArrayConst>() && validWifiStaSlotIndex(slotIndex) &&
        entries[slotIndex].used && sanitizedSsid.length() > 0 && entries[slotIndex].ssid != sanitizedSsid) {
        for (size_t i = 0; i < kWifiStaSlotCount; ++i) {
            if (i == slotIndex || entries[i].used) {
                continue;
            }
            entries[i] = entries[slotIndex];
            entries[slotIndex] = WifiClientSdSecretEntry();
            break;
        }
    }
    for (size_t i = 0; i < kWifiStaSlotCount; ++i) {
        if (!entries[i].used) {
            continue;
        }
        if (i == slotIndex || (sanitizedSsid.length() > 0 && entries[i].ssid == sanitizedSsid)) {
            entries[i] = WifiClientSdSecretEntry();
        }
    }

    if (validWifiStaSlotIndex(slotIndex) && sanitizedSsid.length() > 0) {
        entries[slotIndex].used = true;
        entries[slotIndex].ssid = sanitizedSsid;
        entries[slotIndex].encodedPassword = encodedPassword;
        entries[slotIndex].timestamp = millis();
    }

    return writeWifiClientSdSecretEntries(fs, entries, slotIndex);
}

String loadWifiClientSecretFromSD(const String& expectedSsid, size_t expectedSlotIndex) {
    if (!storageManager.isReady() || !storageManager.isSDCard()) {
        return "";
    }

    StorageManager::SDLockBlocking sdLock(storageManager.getSDMutex());
    if (!sdLock) {
        return "";
    }

    fs::FS* fs = storageManager.getFilesystem();
    if (!fs) {
        return "";
    }

    auto backupFallback = [&]() -> String { return loadWifiClientPasswordObfFromSettingsBackup(fs, expectedSsid); };

    if (!fs->exists(WIFI_CLIENT_SD_SECRET_PATH)) {
        return backupFallback();
    }

    JsonDocument doc;
    if (!loadWifiClientSdSecretDocument(fs, doc)) {
        return backupFallback();
    }

    if (validWifiStaSlotIndex(expectedSlotIndex)) {
        const String slotEncoded = findWifiClientSdSecretInArray(doc, expectedSsid, expectedSlotIndex, true);
        if (slotEncoded.length() > 0) {
            return slotEncoded;
        }
    }

    const String matchingEncoded = findWifiClientSdSecretInArray(doc, expectedSsid, expectedSlotIndex, false);
    if (matchingEncoded.length() > 0) {
        return matchingEncoded;
    }

    String savedSsid = doc[WIFI_CLIENT_SD_SECRET_SSID_KEY] | "";
    if (expectedSsid.length() > 0 && savedSsid.length() > 0 && savedSsid != expectedSsid) {
        Serial.printf("[Settings] WARN: SD WiFi secret SSID mismatch (want='%s' got='%s')\n", expectedSsid.c_str(),
                      savedSsid.c_str());
        return backupFallback();
    }

    const String encoded = doc[WIFI_CLIENT_SD_SECRET_PASSWORD_KEY] | "";
    return encoded.length() > 0 ? encoded : backupFallback();
}

void removeWifiClientSecretFromSD(size_t slotIndex, const String& ssid) {
    if (!storageManager.isReady() || !storageManager.isSDCard()) {
        return;
    }

    StorageManager::SDLockBlocking sdLock(storageManager.getSDMutex());
    if (!sdLock) {
        return;
    }

    fs::FS* fs = storageManager.getFilesystem();
    if (!fs) {
        return;
    }

    WifiClientSdSecretEntry entries[kWifiStaSlotCount];
    JsonDocument existingDoc;
    if (!loadWifiClientSdSecretDocument(fs, existingDoc) || !readWifiClientSdSecretEntries(existingDoc, entries)) {
        return;
    }

    const String sanitizedSsid = sanitizeWifiClientSsidValue(ssid);
    bool changed = false;
    for (size_t i = 0; i < kWifiStaSlotCount; ++i) {
        if (!entries[i].used) {
            continue;
        }
        if (i == slotIndex || (sanitizedSsid.length() > 0 && entries[i].ssid == sanitizedSsid)) {
            entries[i] = WifiClientSdSecretEntry();
            changed = true;
        }
    }

    if (changed) {
        writeWifiClientSdSecretEntries(fs, entries, kWifiStaSlotCount);
    }
}

void clearWifiClientSecretFromSD() {
    if (!storageManager.isReady() || !storageManager.isSDCard()) {
        return;
    }

    StorageManager::SDLockBlocking sdLock(storageManager.getSDMutex());
    if (!sdLock) {
        return;
    }

    fs::FS* fs = storageManager.getFilesystem();
    if (!fs) {
        return;
    }

    if (fs->exists(WIFI_CLIENT_SD_SECRET_PATH)) {
        fs->remove(WIFI_CLIENT_SD_SECRET_PATH);
    }
}

bool storeWifiClientPasswordObfToNvs(const String& encodedPassword, size_t slotIndex) {
    if (encodedPassword.length() == 0 || decodeObfuscatedFromStorage(encodedPassword).length() == 0) {
        return false;
    }
    const char* passwordKey = wifiStaSlotPasswordKey(slotIndex);
    if (!passwordKey) {
        return false;
    }

    Preferences prefs;
    if (!prefs.begin(WIFI_CLIENT_NS, false)) {
        return false;
    }
    const size_t written = prefs.putString(passwordKey, encodedPassword);
    prefs.end();
    return written > 0;
}

int namespaceHealthScore(const char* ns) {
    if (!ns || ns[0] == '\0') {
        return -1;
    }

    Preferences prefs;
    if (!prefs.begin(ns, true)) {
        return -1;
    }

    const int nvsMarker = prefs.getInt(kNvsValid, 0);
    const int settingsVer = prefs.getInt(kNvsSettingsVer, 0);
    int score = 0;

    // Validity marker is the strongest signal that a namespace is current.
    if (nvsMarker > 0)
        score += 1000;
    if (settingsVer > 0)
        score += settingsVer * 10;

    static constexpr const char* kCriticalKeys[] = {kNvsProxyBle, kNvsProxyName, kNvsBrightness, kNvsAutoPush};
    for (const char* key : kCriticalKeys) {
        if (prefs.isKey(key)) {
            score += 5;
        }
    }

    prefs.end();
    return score;
}

bool isKnownSettingsNamespace(const String& ns) {
    return ns == SETTINGS_NS_A || ns == SETTINGS_NS_B || ns == SETTINGS_NS_LEGACY;
}

String SettingsManager::getActiveNamespace() {
    String active = "";
    Preferences meta;
    if (meta.begin(SETTINGS_NS_META, true)) {
        active = meta.getString(kNvsMetaActive, "");
        meta.end();
        if (active.length() > 0 && isKnownSettingsNamespace(active)) {
            // Verify the meta-pointed namespace is actually healthy.
            // If a crash interrupted writeSettingsToNamespace (which clears
            // then rewrites), the namespace could be partial/empty while
            // the OTHER namespace still holds the previous good copy.
            const int activeScore = namespaceHealthScore(active.c_str());
            if (activeScore >= 1000) {
                // nvsValid marker present → write completed fully.
                return active;
            }
            // Meta points to an unhealthy namespace — fall through to
            // health-scoring so we pick the best surviving copy.
            Serial.printf("[Settings] WARN: Meta namespace '%s' unhealthy (score=%d), recovering\n", active.c_str(),
                          activeScore);
        }
    }

    // Meta missing/corrupt: recover by selecting the healthiest settings namespace.
    const int scoreA = namespaceHealthScore(SETTINGS_NS_A);
    const int scoreB = namespaceHealthScore(SETTINGS_NS_B);
    const int scoreLegacy = namespaceHealthScore(SETTINGS_NS_LEGACY);

    String recovered = SETTINGS_NS_LEGACY;
    int bestScore = scoreLegacy;
    if (scoreA > bestScore) {
        recovered = SETTINGS_NS_A;
        bestScore = scoreA;
    }
    if (scoreB > bestScore) {
        recovered = SETTINGS_NS_B;
        bestScore = scoreB;
    }

    if (!isKnownSettingsNamespace(active) && active.length() > 0) {
        Serial.printf("[Settings] WARN: Unknown active namespace '%s', recovering\n", active.c_str());
    }

    if ((recovered == SETTINGS_NS_A || recovered == SETTINGS_NS_B) && recovered != active) {
        Preferences repairMeta;
        if (repairMeta.begin(SETTINGS_NS_META, false)) {
            if (repairMeta.putString(kNvsMetaActive, recovered) > 0) {
                Serial.printf("[Settings] Recovered active namespace to %s\n", recovered.c_str());
            }
            repairMeta.end();
        }
    }

    return recovered;
}

String SettingsManager::getStagingNamespace(const String& activeNamespace) {
    if (activeNamespace == SETTINGS_NS_A)
        return String(SETTINGS_NS_B);
    if (activeNamespace == SETTINGS_NS_B)
        return String(SETTINGS_NS_A);
    return String(SETTINGS_NS_A);
}

bool SettingsManager::writeSettingsToNamespace(const char* ns) {
    settings_.ensureWifiStaSlotForLegacyAlias();

    Preferences prefs;
    if (!prefs.begin(ns, false)) {
        Serial.printf("[Settings] ERROR: Failed to open namespace %s for writing\n", ns);
        return false;
    }

    // Clear old keys in this namespace to avoid stale data from previous versions
    prefs.clear();
    size_t written = 0;
    // Store settings version for migration handling
    written += prefs.putInt(kNvsSettingsVer, SETTINGS_VERSION);
    if (restorePending_) {
        written += prefs.putBool(kNvsRestorePending, true);
    }
    written += prefs.putUInt(kNvsBackupDueRevision, backupDueRevision_);
    written += prefs.putBool(kNvsEnableWifi, settings_.enableWifi);
    // wifiMode is not persisted: it is always derived from wifiClientEnabled
    // in load() (and applyBackupDocument).  Writing it was a no-op read-back.
    written += prefs.putString(kNvsApSsid, settings_.apSSID);
    // Obfuscate passwords before storing
    written += prefs.putString(kNvsApPassword, encodeObfuscatedForStorage(settings_.apPassword));
    // WiFi client (STA) settings - password stored in separate secure namespace
    written += prefs.putBool(kNvsWifiClientEnabled, settings_.wifiClientEnabled);
    for (size_t i = 0; i < kWifiStaSlotCount; ++i) {
        const WifiStaSlot& slot = settings_.wifiStaSlots[i];
        written += prefs.putString(kNvsWifiStaSlotSsid[i], slot.ssid);
        written += prefs.putString(kNvsWifiStaSlotLabel[i], slot.label);
        written += prefs.putUChar(kNvsWifiStaSlotPriority[i], slot.priority);
        written += prefs.putUInt(kNvsWifiStaSlotLastConnected[i], slot.lastConnectedAtSec);
    }
    written += prefs.putBool(kNvsProxyBle, settings_.proxyBLE);
    written += prefs.putString(kNvsProxyName, settings_.proxyName);
    written += prefs.putBool(kNvsDisplayOff, settings_.turnOffDisplay);
    written += prefs.putUChar(kNvsBrightness, settings_.brightness);
    written += prefs.putUShort(kNvsColorBogey, settings_.colorBogey);
    written += prefs.putUShort(kNvsColorFreq, settings_.colorFrequency);
    written += prefs.putUShort(kNvsColorArrowFront, settings_.colorArrowFront);
    written += prefs.putUShort(kNvsColorArrowSide, settings_.colorArrowSide);
    written += prefs.putUShort(kNvsColorArrowRear, settings_.colorArrowRear);
    written += prefs.putUShort(kNvsColorBandLaser, settings_.colorBandL);
    written += prefs.putUShort(kNvsColorBandKa, settings_.colorBandKa);
    written += prefs.putUShort(kNvsColorBandK, settings_.colorBandK);
    written += prefs.putUShort(kNvsColorBandX, settings_.colorBandX);
    written += prefs.putUShort(kNvsColorBandPhoto, settings_.colorBandPhoto);
    written += prefs.putUShort(kNvsColorWifi, settings_.colorWiFiIcon);
    written += prefs.putUShort(kNvsColorWifiConnected, settings_.colorWiFiConnected);
    written += prefs.putUShort(kNvsColorBleConnected, settings_.colorBleConnected);
    written += prefs.putUShort(kNvsColorBleDisconnected, settings_.colorBleDisconnected);
    written += prefs.putUShort(kNvsColorBar1, settings_.colorBar1);
    written += prefs.putUShort(kNvsColorBar2, settings_.colorBar2);
    written += prefs.putUShort(kNvsColorBar3, settings_.colorBar3);
    written += prefs.putUShort(kNvsColorBar4, settings_.colorBar4);
    written += prefs.putUShort(kNvsColorBar5, settings_.colorBar5);
    written += prefs.putUShort(kNvsColorBar6, settings_.colorBar6);
    written += prefs.putUShort(kNvsColorMuted, settings_.colorMuted);
    written += prefs.putUShort(kNvsColorPersisted, settings_.colorPersisted);
    written += prefs.putUShort(kNvsColorVolumeMain, settings_.colorVolumeMain);
    written += prefs.putUShort(kNvsColorVolumeMute, settings_.colorVolumeMute);
    written += prefs.putUShort(kNvsColorRssiV1, settings_.colorRssiV1);
    written += prefs.putUShort(kNvsColorRssiProxy, settings_.colorRssiProxy);
    written += prefs.putUShort(kNvsColorObd, settings_.colorObd);
    written += prefs.putUShort(kNvsColorAlpConn, settings_.colorAlpConnected);
    written += prefs.putUShort(kNvsColorAlpDli, settings_.colorAlpDli);
    written += prefs.putUShort(kNvsColorAlpLid, settings_.colorAlpLidActive);
    written += prefs.putUShort(kNvsColorAlpAlert, settings_.colorAlpAlert);
    written += prefs.putBool(kNvsFreqBandColor, settings_.freqUseBandColor);
    written += prefs.putBool(kNvsHideWifi, settings_.hideWifiIcon);
    written += prefs.putBool(kNvsHideProfile, settings_.hideProfileIndicator);
    written += prefs.putBool(kNvsHideBattery, settings_.hideBatteryIcon);
    written += prefs.putBool(kNvsBatteryPercent, settings_.showBatteryPercent);
    written += prefs.putBool(kNvsHideBle, settings_.hideBleIcon);
    written += prefs.putBool(kNvsHideVolume, settings_.hideVolumeIndicator);
    written += prefs.putBool(kNvsHideRssi, settings_.hideRssiIndicator);
    written += prefs.putUChar(kNvsVoiceMode, (uint8_t)settings_.voiceAlertMode);
    written += prefs.putBool(kNvsVoiceDirection, settings_.voiceDirectionEnabled);
    written += prefs.putBool(kNvsVoiceBogeys, settings_.announceBogeyCount);
    written += prefs.putBool(kNvsMuteVoiceAtVol0, settings_.muteVoiceIfVolZero);
    written += prefs.putUChar(kNvsVoiceVolume, settings_.voiceVolume);
    written += prefs.putBool(kNvsSecondaryAlerts, settings_.announceSecondaryAlerts);
    written += prefs.putBool(kNvsSecondaryLaser, settings_.secondaryLaser);
    written += prefs.putBool(kNvsSecondaryKa, settings_.secondaryKa);
    written += prefs.putBool(kNvsSecondaryK, settings_.secondaryK);
    written += prefs.putBool(kNvsSecondaryX, settings_.secondaryX);
    written += prefs.putBool(kNvsVolFadeEnabled, settings_.alertVolumeFadeEnabled);
    written += prefs.putUChar(kNvsVolFadeSeconds, settings_.alertVolumeFadeDelaySec);
    written += prefs.putUChar(kNvsVolFadeVolume, settings_.alertVolumeFadeVolume);
    written += prefs.putBool(kNvsSpeedMuteEnabled, settings_.speedMuteEnabled);
    written += prefs.putUChar(kNvsSpeedMuteThreshold, settings_.speedMuteThresholdMph);
    written += prefs.putUChar(kNvsSpeedMuteHysteresis, settings_.speedMuteHysteresisMph);
    written += prefs.putUChar(kNvsSpeedMuteVolume, settings_.speedMuteVolume);
    written += prefs.putBool(kNvsSpeedMuteVoice, settings_.speedMuteVoice);
    written += prefs.putBool(kNvsStealthEnabled, settings_.stealthEnabled);
    written += prefs.putBool(kNvsAutoPush, settings_.autoPushEnabled);
    written += prefs.putInt(kNvsActiveSlot, settings_.activeSlot);
    written += prefs.putString(kNvsSlot0Name, settings_.slot0Name);
    written += prefs.putString(kNvsSlot1Name, settings_.slot1Name);
    written += prefs.putString(kNvsSlot2Name, settings_.slot2Name);
    written += prefs.putUShort(kNvsSlot0Color, settings_.slot0Color);
    written += prefs.putUShort(kNvsSlot1Color, settings_.slot1Color);
    written += prefs.putUShort(kNvsSlot2Color, settings_.slot2Color);
    written += prefs.putUChar(kNvsSlot0Volume, settings_.slot0Volume);
    written += prefs.putUChar(kNvsSlot1Volume, settings_.slot1Volume);
    written += prefs.putUChar(kNvsSlot2Volume, settings_.slot2Volume);
    written += prefs.putUChar(kNvsSlot0MuteVolume, settings_.slot0MuteVolume);
    written += prefs.putUChar(kNvsSlot1MuteVolume, settings_.slot1MuteVolume);
    written += prefs.putUChar(kNvsSlot2MuteVolume, settings_.slot2MuteVolume);
    written += prefs.putBool(kNvsSlot0DarkMode, settings_.slot0DarkMode);
    written += prefs.putBool(kNvsSlot1DarkMode, settings_.slot1DarkMode);
    written += prefs.putBool(kNvsSlot2DarkMode, settings_.slot2DarkMode);
    written += prefs.putBool(kNvsSlot0MuteToZero, settings_.slot0MuteToZero);
    written += prefs.putBool(kNvsSlot1MuteToZero, settings_.slot1MuteToZero);
    written += prefs.putBool(kNvsSlot2MuteToZero, settings_.slot2MuteToZero);
    written += prefs.putUChar(kNvsSlot0Persistence, settings_.slot0AlertPersist);
    written += prefs.putUChar(kNvsSlot1Persistence, settings_.slot1AlertPersist);
    written += prefs.putUChar(kNvsSlot2Persistence, settings_.slot2AlertPersist);
    written += prefs.putBool(kNvsSlot0PriorityArrow, settings_.slot0PriorityArrow);
    written += prefs.putBool(kNvsSlot1PriorityArrow, settings_.slot1PriorityArrow);
    written += prefs.putBool(kNvsSlot2PriorityArrow, settings_.slot2PriorityArrow);
    written += prefs.putString(kNvsSlot0Profile, settings_.slot0_default.profileName);
    written += prefs.putInt(kNvsSlot0Mode, settings_.slot0_default.mode);
    written += prefs.putString(kNvsSlot1Profile, settings_.slot1_highway.profileName);
    written += prefs.putInt(kNvsSlot1Mode, settings_.slot1_highway.mode);
    written += prefs.putString(kNvsSlot2Profile, settings_.slot2_comfort.profileName);
    written += prefs.putInt(kNvsSlot2Mode, settings_.slot2_comfort.mode);
    written += prefs.putString(kNvsLastV1Address, settings_.lastV1Address);
    written += prefs.putUChar(kNvsAutoPowerOff, settings_.autoPowerOffMinutes);
    written += prefs.putUChar(kNvsApTimeout, settings_.apTimeoutMinutes);

    // OBD settings
    written += prefs.putBool(kNvsObdEnabled, settings_.obdEnabled);
    written += prefs.putString(kNvsObdAddress, settings_.obdSavedAddress);
    written += prefs.putString(kNvsObdName, settings_.obdSavedName);
    written += prefs.putUChar(kNvsObdAddressType, settings_.obdSavedAddrType);
    written += prefs.putChar(kNvsObdMinRssi, settings_.obdMinRssi);
    written += prefs.putUInt(kNvsCycleObdScanWindow, settings_.obdScanWindowMs);
    written += prefs.putUInt(kNvsCycleObdRetryInt, settings_.obdRetryIntervalMs);
    written += prefs.putUInt(kNvsCycleProxyOpenWindow, settings_.proxyOpenWindowMs);
    written += prefs.putUInt(kNvsCycleWifiOpenTimeout, settings_.wifiOpenTimeoutMs);
    written += prefs.putUInt(kNvsCycleV1SettleQuiet, settings_.v1SettleQuietMs);
    written += prefs.putUInt(kNvsCycleV1SettleFallback, settings_.v1SettleFallbackMs);
    written += prefs.putUInt(kNvsCycleTeardownAckTimeout, settings_.cycleTeardownAckTimeoutMs);

    // ALP settings
    written += prefs.putBool(kNvsAlpEnabled, settings_.alpEnabled);
    written += prefs.putBool(kNvsAlpSdLog, settings_.alpSdLogEnabled);
    written += prefs.putUChar(kNvsAlpPersistSec, std::min<uint8_t>(5, settings_.alpAlertPersistSec));
    written += prefs.putBool(kNvsAlpNoV1Laser, settings_.alpDisableV1LaserOnPush);

    // Debug / diagnostics
    written += prefs.putBool(kNvsPowerOffSdLog, settings_.powerOffSdLog);

    // GPS settings
    written += prefs.putBool(kNvsGpsEnabled, settings_.gpsEnabled);
    written += prefs.putUInt(kNvsGpsBaud, settings_.gpsBaud);
    written += prefs.putBool(kNvsGpsEnablePolarity, settings_.gpsEnablePinActiveHigh);
    written += prefs.putBool(kNvsGpsLogUtcToPerf, settings_.gpsLogUtcToPerf);
    written += prefs.putBool(kNvsGpsLogUtcToAlp, settings_.gpsLogUtcToAlp);

    // NVS validity marker - used to detect if NVS was wiped.
    // Written LAST so its presence proves the entire write completed.
    written += prefs.putInt(kNvsValid, SETTINGS_VERSION);

    // Verify the marker was actually persisted.  If NVS ran out of
    // entries/pages, later keys silently fail and the namespace would
    // appear incomplete on the next boot.
    const int verifyMarker = prefs.getInt(kNvsValid, 0);
    prefs.end();

    if (verifyMarker != SETTINGS_VERSION) {
        Serial.printf("[Settings] ERROR: nvsValid verify failed in %s (expected %d, got %d) — written=%d\n", ns,
                      SETTINGS_VERSION, verifyMarker, (int)written);
        return false;
    }

    Serial.printf("[Settings] Wrote %d bytes to namespace %s\n", (int)written, ns);
    return true;
}

bool SettingsManager::persistSettingsAtomically() {
    String activeNs = getActiveNamespace();
    String stagingNs = getStagingNamespace(activeNs);

    if (!writeSettingsToNamespace(stagingNs.c_str())) {
        // First attempt failed - try NVS recovery and retry once
        Serial.println("[Settings] First write attempt failed, trying NVS recovery...");
        attemptNvsRecovery(activeNs.c_str());

        if (!writeSettingsToNamespace(stagingNs.c_str())) {
            Serial.println("[Settings] ERROR: Failed to write staging settings_ even after recovery");
            return false;
        }
    }

    Preferences meta;
    if (!meta.begin(SETTINGS_NS_META, false)) {
        Serial.println("[Settings] ERROR: Failed to open settings_ meta namespace");
        Serial.printf("[Settings] WARN: Falling back to in-place write on %s\n", activeNs.c_str());
        if (!writeSettingsToNamespace(activeNs.c_str())) {
            Serial.println("[Settings] ERROR: In-place fallback write failed");
            return false;
        }
        Serial.printf("[Settings] Fallback write succeeded in %s\n", activeNs.c_str());
        return true;
    }

    bool committed = meta.putString(kNvsMetaActive, stagingNs) > 0;
    meta.end();

    if (!committed) {
        Serial.println("[Settings] ERROR: Failed to update active settings_ namespace");
        Serial.printf("[Settings] WARN: Falling back to in-place write on %s\n", activeNs.c_str());
        if (!writeSettingsToNamespace(activeNs.c_str())) {
            Serial.println("[Settings] ERROR: In-place fallback write failed");
            return false;
        }
        Serial.printf("[Settings] Fallback write succeeded in %s\n", activeNs.c_str());
        return true;
    }

    Serial.printf("[Settings] Active namespace advanced from %s to %s\n", activeNs.c_str(), stagingNs.c_str());
    return true;
}

// --- WiFi client credential methods ---

String SettingsManager::getWifiStaSlotPassword(size_t index) {
    if (!validWifiStaSlotIndex(index)) {
        return "";
    }
    const char* passwordKey = wifiStaSlotPasswordKey(index);
    if (!passwordKey) {
        return "";
    }

    Preferences prefs;
    bool hasNvsKey = false;
    String storedPwd;
    bool legacyKeyPresent = false;
    String legacyStoredPwd;
    if (!prefs.begin(WIFI_CLIENT_NS, true)) { // Read-only
        storedPwd = "";
    } else {
        hasNvsKey = prefs.isKey(passwordKey);
        if (hasNvsKey) {
            storedPwd = prefs.getString(passwordKey, "");
        } else if (index == 0) {
            legacyKeyPresent = prefs.isKey(kNvsWifiPassword);
            if (legacyKeyPresent) {
                legacyStoredPwd = prefs.getString(kNvsWifiPassword, "");
            }
        }
        prefs.end();
    }

    if (!hasNvsKey && legacyKeyPresent) {
        hasNvsKey = true;
        storedPwd = legacyStoredPwd;
    }

    // Open-network credential: key present with empty value is valid.
    if (hasNvsKey && storedPwd.length() == 0) {
        return "";
    }

    if (storedPwd.length() > 0) {
        // Password is stored as obfuscated hex payload (legacy raw XOR still supported).
        String decoded = decodeObfuscatedFromStorage(storedPwd);
        if (decoded.length() == 0) {
            Serial.println("[Settings] WARN: WiFi password decode returned empty for non-empty stored value — possible "
                           "NVS corruption");
        }
        return decoded;
    }

    // Fallback: recover password from SD-backed secret store if available.
    const String expectedSsid = settings_.wifiStaSlots[index].ssid;
    String sdEncoded = loadWifiClientSecretFromSD(expectedSsid, index);
    if (sdEncoded.length() == 0) {
        return "";
    }

    String decoded = decodeObfuscatedFromStorage(sdEncoded);
    if (decoded.length() == 0) {
        return "";
    }

    // Heal NVS from SD fallback so future reconnects do not hit SD.
    Preferences healPrefs;
    if (healPrefs.begin(WIFI_CLIENT_NS, false)) {
        healPrefs.putString(passwordKey, sdEncoded);
        healPrefs.end();
        Serial.println("[Settings] Recovered WiFi client password from SD credential backup");
    }

    return decoded;
}

String SettingsManager::getWifiClientPassword() {
    settings_.ensureWifiStaSlotForLegacyAlias();
    const int index = settings_.primaryWifiStaSlotIndex();
    return index >= 0 ? getWifiStaSlotPassword(static_cast<size_t>(index)) : "";
}

void SettingsManager::setWifiClientEnabled(bool enabled) {
    settings_.wifiClientEnabled = enabled;
    settings_.wifiMode = enabled ? V1_WIFI_APSTA : V1_WIFI_AP;
    save();
}

void SettingsManager::setWifiStaSlotCredentials(size_t index, const String& ssid, const String& password,
                                                const String& label, uint8_t priority) {
    if (!validWifiStaSlotIndex(index)) {
        return;
    }

    WifiStaSlot& slot = settings_.wifiStaSlots[index];
    slot.ssid = sanitizeWifiClientSsidValue(ssid);
    slot.label = sanitizeWifiStaSlotLabelValue(label);
    if (slot.label.length() == 0 && slot.ssid.length() > 0) {
        slot.label = (index == 0) ? "Saved" : slot.ssid;
    }
    slot.priority = priority;
    if (slot.ssid.length() == 0) {
        slot.label = "";
        slot.lastConnectedAtSec = 0;
    }
    settings_.wifiClientEnabled = settings_.hasConfiguredWifiStaSlot();
    settings_.refreshWifiClientAliasFromSlots();
    settings_.wifiMode = settings_.wifiClientEnabled ? V1_WIFI_APSTA : V1_WIFI_AP;

    const String sanitizedPassword = sanitizeWifiClientPasswordValue(password);
    const String encodedPassword = encodeObfuscatedForStorage(sanitizedPassword);
    bool nvsSaved = false;
    const char* passwordKey = wifiStaSlotPasswordKey(index);

    // Store password in separate namespace with obfuscation
    Preferences prefs;
    if (passwordKey && prefs.begin(WIFI_CLIENT_NS, false)) { // Read-write
        size_t written = 0;
        if (sanitizedPassword.length() == 0) {
            // Open network: no password required.
            prefs.remove(passwordKey);
            if (index == 0) {
                prefs.remove(kNvsWifiPassword);
            }
            nvsSaved = true;
        } else {
            written = prefs.putString(passwordKey, encodedPassword);
            nvsSaved = written > 0;
        }
        prefs.end();

        if (nvsSaved) {
            Serial.println("[Settings] WiFi client credentials saved");
        } else {
            // NVS might be full - try recovery and retry
            Serial.println("[Settings] WiFi password save failed, trying NVS recovery...");
            String activeNs = getActiveNamespace();
            attemptNvsRecovery(activeNs.c_str());

            // Retry save
            if (prefs.begin(WIFI_CLIENT_NS, false)) {
                if (sanitizedPassword.length() == 0) {
                    prefs.remove(passwordKey);
                    if (index == 0) {
                        prefs.remove(kNvsWifiPassword);
                    }
                    nvsSaved = true;
                } else {
                    written = prefs.putString(passwordKey, encodedPassword);
                    nvsSaved = written > 0;
                }
                prefs.end();
                if (nvsSaved) {
                    Serial.println("[Settings] WiFi client credentials saved after recovery");
                } else {
                    Serial.println("[Settings] ERROR: WiFi password save failed even after recovery");
                }
            }
        }
    } else {
        Serial.println("[Settings] ERROR: Failed to open WiFi client namespace");
    }

    // Redundant SD copy for recovery when NVS gets wiped/corrupted.
    if (slot.ssid.length() > 0 && saveWifiClientSecretToSD(index, slot.ssid, encodedPassword)) {
        Serial.println("[Settings] WiFi client secret mirrored to SD");
    }

    save();
}

void SettingsManager::setWifiClientCredentials(const String& ssid, const String& password) {
    setWifiStaSlotCredentials(0, ssid, password, settings_.wifiStaSlots[0].label, 0);
}

void SettingsManager::markWifiStaSlotConnected(size_t index, uint32_t connectedAtSec) {
    if (!validWifiStaSlotIndex(index) || !settings_.wifiStaSlots[index].isConfigured()) {
        return;
    }
    settings_.wifiStaSlots[index].lastConnectedAtSec = connectedAtSec;
    settings_.refreshWifiClientAliasFromSlots();
    save();
}

void SettingsManager::clearWifiStaSlot(size_t index) {
    if (!validWifiStaSlotIndex(index)) {
        return;
    }
    const String removedSsid = settings_.wifiStaSlots[index].ssid;
    settings_.wifiStaSlots[index] = WifiStaSlot();
    settings_.wifiClientEnabled = settings_.hasConfiguredWifiStaSlot();
    settings_.refreshWifiClientAliasFromSlots();
    settings_.wifiMode = settings_.wifiClientEnabled ? V1_WIFI_APSTA : V1_WIFI_AP;

    const char* passwordKey = wifiStaSlotPasswordKey(index);
    Preferences prefs;
    if (passwordKey && prefs.begin(WIFI_CLIENT_NS, false)) {
        prefs.remove(passwordKey);
        if (index == 0) {
            prefs.remove(kNvsWifiPassword);
        }
        prefs.end();
    }

    if (!settings_.hasConfiguredWifiStaSlot()) {
        clearWifiClientSecretFromSD();
    } else {
        removeWifiClientSecretFromSD(index, removedSsid);
    }

    save();
}

void SettingsManager::clearWifiClientCredentials() {
    for (size_t i = 0; i < kWifiStaSlotCount; ++i) {
        settings_.wifiStaSlots[i] = WifiStaSlot();
    }
    settings_.wifiClientSSID = "";
    settings_.wifiClientEnabled = false;
    settings_.wifiMode = V1_WIFI_AP;

    // Clear the passwords from secure namespace
    Preferences prefs;
    if (prefs.begin(WIFI_CLIENT_NS, false)) {
        prefs.clear();
        prefs.end();
        Serial.println("[Settings] WiFi client credentials cleared");
    }

    clearWifiClientSecretFromSD();

    save();
}
