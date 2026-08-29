/**
 * Settings NVS persistence, credential obfuscation, and WiFi credentials.
 */

#include "settings_internals.h"
#include "display_visual_contract.h"

// --- NVS recovery, crypto, WiFi SD secret helpers ---

namespace {

constexpr uint32_t LAST_V1_FALLBACK_DEBOUNCE_MS = 750;
constexpr uint32_t LAST_V1_FALLBACK_RETRY_MS = 1000;
constexpr const char* WIFI_CLIENT_BACKUP_PASSWORD_KEY = "wifiClientPasswordObf";
constexpr const char* WIFI_STA_SLOT_BACKUP_PASSWORD_KEY = "passwordObf";
constexpr const char* WIFI_CLIENT_SD_SECRETS_KEY = "secrets";
constexpr const char* WIFI_CLIENT_SD_SECRET_INDEX_KEY = "index";
constexpr const char* WIFI_CLIENT_SD_SECRET_SSID_KEY = "ssid";
constexpr const char* WIFI_CLIENT_SD_SECRET_PASSWORD_KEY = "password_obf";
constexpr const char* WIFI_CLIENT_SD_SECRET_TIMESTAMP_KEY = "timestamp";
constexpr const char* WIFI_CLIENT_SD_SECRET_TEMP_PATH = "/v1wifi_secret.json.tmp";

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

bool wifiClientSdSecretDocumentIsValid(const JsonDocument& doc) {
    if (!wifiClientSdSecretTypeMatches(doc) || !doc["_version"].is<int>() ||
        doc["_version"].as<int>() != WIFI_CLIENT_SD_SECRET_VERSION ||
        !doc[WIFI_CLIENT_SD_SECRETS_KEY].is<JsonArrayConst>()) {
        return false;
    }

    bool seen[kWifiStaSlotCount] = {};
    for (JsonObjectConst entry : doc[WIFI_CLIENT_SD_SECRETS_KEY].as<JsonArrayConst>()) {
        if (!entry[WIFI_CLIENT_SD_SECRET_INDEX_KEY].is<int>() ||
            !entry[WIFI_CLIENT_SD_SECRET_SSID_KEY].is<const char*>() ||
            !entry[WIFI_CLIENT_SD_SECRET_PASSWORD_KEY].is<const char*>()) {
            return false;
        }
        const int rawIndex = entry[WIFI_CLIENT_SD_SECRET_INDEX_KEY].as<int>();
        if (rawIndex < 0 || rawIndex >= static_cast<int>(kWifiStaSlotCount) || seen[rawIndex]) {
            return false;
        }
        seen[rawIndex] = true;
        const String rawSsid = entry[WIFI_CLIENT_SD_SECRET_SSID_KEY].as<String>();
        if (rawSsid.length() == 0 || sanitizeWifiClientSsidValue(rawSsid) != rawSsid) {
            return false;
        }
        const String encoded = entry[WIFI_CLIENT_SD_SECRET_PASSWORD_KEY].as<String>();
        if (encoded.length() > 0 && decodeObfuscatedFromStorage(encoded).length() == 0) {
            return false;
        }
    }
    return true;
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
        if (!fs->exists(WIFI_CLIENT_SD_SECRET_PATH)) {
            return true;
        }
        const String rollbackPath = StorageManager::rollbackPathFor(WIFI_CLIENT_SD_SECRET_PATH);
        if (fs->exists(rollbackPath.c_str()) && !fs->remove(rollbackPath.c_str())) {
            return false;
        }
        if (!fs->rename(WIFI_CLIENT_SD_SECRET_PATH, rollbackPath.c_str())) {
            return false;
        }
        fs->remove(rollbackPath.c_str());
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

    if (!wifiClientSdSecretDocumentIsValid(doc)) {
        return false;
    }

    if (fs->exists(WIFI_CLIENT_SD_SECRET_TEMP_PATH)) {
        fs->remove(WIFI_CLIENT_SD_SECRET_TEMP_PATH);
    }
    File file = fs->open(WIFI_CLIENT_SD_SECRET_TEMP_PATH, FILE_WRITE);
    if (!file) {
        Serial.println("[Settings] WARN: Failed to open SD WiFi secret file for write");
        return false;
    }

    const size_t expectedBytes = measureJson(doc);
    const size_t writtenBytes = serializeJson(doc, file);
    file.flush();
    file.close();
    if (writtenBytes != expectedBytes) {
        fs->remove(WIFI_CLIENT_SD_SECRET_TEMP_PATH);
        Serial.println("[Settings] WARN: Short write while staging SD WiFi secret");
        return false;
    }

    JsonDocument candidate;
    File verifyFile = fs->open(WIFI_CLIENT_SD_SECRET_TEMP_PATH, FILE_READ);
    const DeserializationError verifyError = verifyFile ? deserializeJson(candidate, verifyFile)
                                                        : DeserializationError::InvalidInput;
    if (verifyFile) {
        verifyFile.close();
    }
    if (verifyError || !wifiClientSdSecretDocumentIsValid(candidate)) {
        fs->remove(WIFI_CLIENT_SD_SECRET_TEMP_PATH);
        Serial.println("[Settings] WARN: SD WiFi secret candidate validation failed");
        return false;
    }

    return StorageManager::promoteTempFileWithRollback(*fs, WIFI_CLIENT_SD_SECRET_TEMP_PATH,
                                                       WIFI_CLIENT_SD_SECRET_PATH);
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

struct WifiPasswordNvsSnapshot {
    bool slotPresent = false;
    String slotValue;
    bool legacyPresent = false;
    String legacyValue;
};

bool readWifiPasswordNvsSnapshot(size_t slotIndex, WifiPasswordNvsSnapshot& snapshot) {
    const char* passwordKey = wifiStaSlotPasswordKey(slotIndex);
    Preferences prefs;
    if (!passwordKey || !prefs.begin(WIFI_CLIENT_NS, true)) {
        return false;
    }
    snapshot.slotPresent = prefs.isKey(passwordKey);
    if (snapshot.slotPresent) {
        snapshot.slotValue = prefs.getString(passwordKey, "");
    }
    if (slotIndex == 0) {
        snapshot.legacyPresent = prefs.isKey(kNvsWifiPassword);
        if (snapshot.legacyPresent) {
            snapshot.legacyValue = prefs.getString(kNvsWifiPassword, "");
        }
    }
    prefs.end();
    return true;
}

bool writeWifiPasswordKeyState(Preferences& prefs, const char* key, bool present, const String& value) {
    if (!key) {
        return false;
    }
    if (!present) {
        if (prefs.isKey(key) && !prefs.remove(key)) {
            return false;
        }
        return !prefs.isKey(key);
    }
    const size_t written = prefs.putString(key, value);
    return written == value.length() && prefs.isKey(key) && prefs.getString(key, "") == value;
}

bool restoreWifiPasswordNvsSnapshot(size_t slotIndex, const WifiPasswordNvsSnapshot& snapshot) {
    const char* passwordKey = wifiStaSlotPasswordKey(slotIndex);
    Preferences prefs;
    if (!passwordKey || !prefs.begin(WIFI_CLIENT_NS, false)) {
        return false;
    }
    bool restored = writeWifiPasswordKeyState(prefs, passwordKey, snapshot.slotPresent, snapshot.slotValue);
    if (slotIndex == 0) {
        restored = writeWifiPasswordKeyState(prefs, kNvsWifiPassword, snapshot.legacyPresent,
                                             snapshot.legacyValue) &&
                   restored;
    }
    prefs.end();
    return restored;
}

bool storeWifiPasswordCandidate(size_t slotIndex, const String& encodedPassword) {
    if (encodedPassword.length() > 0 && decodeObfuscatedFromStorage(encodedPassword).length() == 0) {
        return false;
    }
    const char* passwordKey = wifiStaSlotPasswordKey(slotIndex);
    Preferences prefs;
    if (!passwordKey || !prefs.begin(WIFI_CLIENT_NS, false)) {
        return false;
    }
    const bool present = encodedPassword.length() > 0;
    bool stored = writeWifiPasswordKeyState(prefs, passwordKey, present, encodedPassword);
    if (slotIndex == 0) {
        stored = writeWifiPasswordKeyState(prefs, kNvsWifiPassword, false, "") && stored;
    }
    prefs.end();
    return stored;
}

struct WifiCredentialJournal {
    size_t slotIndex = 0;
    String oldSsid;
    String oldEncodedPassword;
    String newSsid;
    String newEncodedPassword;
};

enum class WifiCredentialJournalMode : uint8_t {
    SingleSlot = 0,
    ForgetAll = 1,
};

struct WifiForgetAllJournal {
    String oldSsid[kWifiStaSlotCount];
    String oldEncodedPassword[kWifiStaSlotCount];
};

bool wifiCredentialJournalPresent() {
    Preferences prefs;
    if (!prefs.begin(WIFI_CLIENT_NS, true)) {
        return false;
    }
    const bool present = prefs.getBool(kNvsWifiTxnReady, false);
    prefs.end();
    return present;
}

WifiCredentialJournalMode wifiCredentialJournalMode() {
    Preferences prefs;
    if (!prefs.begin(WIFI_CLIENT_NS, true)) {
        return WifiCredentialJournalMode::SingleSlot;
    }
    const WifiCredentialJournalMode mode = static_cast<WifiCredentialJournalMode>(
        prefs.getUChar(kNvsWifiTxnMode, static_cast<uint8_t>(WifiCredentialJournalMode::SingleSlot)));
    prefs.end();
    return mode;
}

bool writeWifiCredentialJournal(const WifiCredentialJournal& journal) {
    if (!validWifiStaSlotIndex(journal.slotIndex)) {
        return false;
    }
    Preferences prefs;
    if (!prefs.begin(WIFI_CLIENT_NS, false)) {
        return false;
    }
    if (prefs.isKey(kNvsWifiTxnReady) && !prefs.remove(kNvsWifiTxnReady)) {
        prefs.end();
        return false;
    }
    bool written = prefs.putUChar(kNvsWifiTxnMode, static_cast<uint8_t>(WifiCredentialJournalMode::SingleSlot)) ==
                   sizeof(uint8_t);
    written = prefs.putUChar(kNvsWifiTxnSlot, static_cast<uint8_t>(journal.slotIndex)) == sizeof(uint8_t) && written;
    written = writeWifiPasswordKeyState(prefs, kNvsWifiTxnOldSsid, true, journal.oldSsid) && written;
    written = writeWifiPasswordKeyState(prefs, kNvsWifiTxnOldPass, true, journal.oldEncodedPassword) && written;
    written = writeWifiPasswordKeyState(prefs, kNvsWifiTxnNewSsid, true, journal.newSsid) && written;
    written = writeWifiPasswordKeyState(prefs, kNvsWifiTxnNewPass, true, journal.newEncodedPassword) && written;
    if (written) {
        written = prefs.putBool(kNvsWifiTxnReady, true) == sizeof(bool) &&
                  prefs.getBool(kNvsWifiTxnReady, false);
    }
    prefs.end();
    return written;
}

bool readWifiCredentialJournal(WifiCredentialJournal& journal) {
    Preferences prefs;
    if (!prefs.begin(WIFI_CLIENT_NS, true)) {
        return false;
    }
    if (!prefs.getBool(kNvsWifiTxnReady, false)) {
        prefs.end();
        return false;
    }
    if (prefs.getUChar(kNvsWifiTxnMode, static_cast<uint8_t>(WifiCredentialJournalMode::SingleSlot)) !=
        static_cast<uint8_t>(WifiCredentialJournalMode::SingleSlot)) {
        prefs.end();
        return false;
    }
    const uint8_t slotIndex = prefs.getUChar(kNvsWifiTxnSlot, static_cast<uint8_t>(kWifiStaSlotCount));
    journal.slotIndex = slotIndex;
    journal.oldSsid = prefs.getString(kNvsWifiTxnOldSsid, "");
    journal.oldEncodedPassword = prefs.getString(kNvsWifiTxnOldPass, "");
    journal.newSsid = prefs.getString(kNvsWifiTxnNewSsid, "");
    journal.newEncodedPassword = prefs.getString(kNvsWifiTxnNewPass, "");
    prefs.end();

    return validWifiStaSlotIndex(journal.slotIndex) &&
           sanitizeWifiClientSsidValue(journal.oldSsid) == journal.oldSsid &&
           sanitizeWifiClientSsidValue(journal.newSsid) == journal.newSsid &&
           (journal.oldEncodedPassword.length() == 0 ||
            decodeObfuscatedFromStorage(journal.oldEncodedPassword).length() > 0) &&
           (journal.newEncodedPassword.length() == 0 ||
            decodeObfuscatedFromStorage(journal.newEncodedPassword).length() > 0);
}

bool writeWifiForgetAllJournal(const WifiForgetAllJournal& journal) {
    JsonDocument doc;
    JsonArray slots = doc["slots"].to<JsonArray>();
    for (size_t index = 0; index < kWifiStaSlotCount; ++index) {
        JsonObject slot = slots.add<JsonObject>();
        slot["index"] = index;
        slot["ssid"] = journal.oldSsid[index];
        slot["password"] = journal.oldEncodedPassword[index];
    }
    String payload;
    serializeJson(doc, payload);

    Preferences prefs;
    if (!prefs.begin(WIFI_CLIENT_NS, false)) {
        return false;
    }
    if (prefs.isKey(kNvsWifiTxnReady) && !prefs.remove(kNvsWifiTxnReady)) {
        prefs.end();
        return false;
    }
    bool written = prefs.putUChar(kNvsWifiTxnMode, static_cast<uint8_t>(WifiCredentialJournalMode::ForgetAll)) ==
                   sizeof(uint8_t);
    written = prefs.putString(kNvsWifiTxnData, payload) == payload.length() &&
              prefs.getString(kNvsWifiTxnData, "") == payload && written;
    if (written) {
        written = prefs.putBool(kNvsWifiTxnReady, true) == sizeof(bool) &&
                  prefs.getBool(kNvsWifiTxnReady, false);
    }
    prefs.end();
    return written;
}

bool readWifiForgetAllJournal(WifiForgetAllJournal& journal) {
    Preferences prefs;
    if (!prefs.begin(WIFI_CLIENT_NS, true)) {
        return false;
    }
    if (!prefs.getBool(kNvsWifiTxnReady, false) ||
        prefs.getUChar(kNvsWifiTxnMode, static_cast<uint8_t>(WifiCredentialJournalMode::SingleSlot)) !=
            static_cast<uint8_t>(WifiCredentialJournalMode::ForgetAll)) {
        prefs.end();
        return false;
    }
    const String payload = prefs.getString(kNvsWifiTxnData, "");
    prefs.end();

    JsonDocument doc;
    if (deserializeJson(doc, payload) || !doc["slots"].is<JsonArrayConst>() ||
        doc["slots"].size() != kWifiStaSlotCount) {
        return false;
    }
    bool seen[kWifiStaSlotCount] = {};
    for (JsonObjectConst slot : doc["slots"].as<JsonArrayConst>()) {
        if (!slot["index"].is<int>() || !slot["ssid"].is<const char*>() ||
            !slot["password"].is<const char*>()) {
            return false;
        }
        const int rawIndex = slot["index"].as<int>();
        if (rawIndex < 0 || rawIndex >= static_cast<int>(kWifiStaSlotCount) || seen[rawIndex]) {
            return false;
        }
        seen[rawIndex] = true;
        const size_t index = static_cast<size_t>(rawIndex);
        journal.oldSsid[index] = slot["ssid"].as<String>();
        journal.oldEncodedPassword[index] = slot["password"].as<String>();
        if (sanitizeWifiClientSsidValue(journal.oldSsid[index]) != journal.oldSsid[index] ||
            (journal.oldEncodedPassword[index].length() > 0 &&
             decodeObfuscatedFromStorage(journal.oldEncodedPassword[index]).length() == 0)) {
            return false;
        }
    }
    return true;
}

bool clearWifiCredentialJournal() {
    Preferences prefs;
    if (!prefs.begin(WIFI_CLIENT_NS, false)) {
        return false;
    }
    const bool markerRemoved = !prefs.isKey(kNvsWifiTxnReady) || prefs.remove(kNvsWifiTxnReady);
    const bool cleared = markerRemoved && !prefs.isKey(kNvsWifiTxnReady);
    if (cleared) {
        prefs.remove(kNvsWifiTxnSlot);
        prefs.remove(kNvsWifiTxnOldSsid);
        prefs.remove(kNvsWifiTxnOldPass);
        prefs.remove(kNvsWifiTxnNewSsid);
        prefs.remove(kNvsWifiTxnNewPass);
        prefs.remove(kNvsWifiTxnMode);
        prefs.remove(kNvsWifiTxnData);
    }
    prefs.end();
    return cleared;
}

bool restoreAllWifiPasswordSnapshots(const WifiPasswordNvsSnapshot (&snapshots)[kWifiStaSlotCount]) {
    bool restored = true;
    for (size_t index = 0; index < kWifiStaSlotCount; ++index) {
        restored = restoreWifiPasswordNvsSnapshot(index, snapshots[index]) && restored;
    }
    return restored;
}

bool writeWifiSecretStateFromSettings(StorageManager& storage, const V1Settings& settings,
                                      const String (&encodedPasswords)[kWifiStaSlotCount]) {
    if (!storage.isReady() || !storage.isSDCard()) {
        return true;
    }
    StorageManager::SDLockBlocking lock(storage.getSDMutex(), /*checkDmaHeap=*/true);
    if (!lock) {
        return false;
    }
    fs::FS* fs = storage.getFilesystem();
    if (!fs) {
        return false;
    }
    WifiClientSdSecretEntry entries[kWifiStaSlotCount];
    for (size_t index = 0; index < kWifiStaSlotCount; ++index) {
        if (!settings.wifiStaSlots[index].isConfigured()) {
            continue;
        }
        entries[index].used = true;
        entries[index].ssid = settings.wifiStaSlots[index].ssid;
        entries[index].encodedPassword = encodedPasswords[index];
        entries[index].timestamp = millis();
    }
    return writeWifiClientSdSecretEntries(fs, entries, kWifiStaSlotCount);
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

bool saveWifiClientSecretToSD(StorageManager& storage, size_t slotIndex, const String& ssid,
                              const String& encodedPassword) {
    if (!storage.isReady() || !storage.isSDCard()) {
        return false;
    }

    // checkDmaHeap=true: WiFi client secrets are written from route handlers
    // dispatched inside wifiManager.process() (main.cpp:620), so the radio is
    // up here. See the WHO PAYS FOR THIS note in storage_manager.h.
    StorageManager::SDLockBlocking sdLock(storage.getSDMutex(), /*checkDmaHeap=*/true);
    if (!sdLock) {
        Serial.println("[Settings] WARN: Failed to acquire SD mutex for WiFi secret save");
        return false;
    }

    fs::FS* fs = storage.getFilesystem();
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

String loadWifiClientSecretFromSD(StorageManager& storage, const String& expectedSsid,
                                  size_t expectedSlotIndex) {
    if (!storage.isReady() || !storage.isSDCard()) {
        return "";
    }

    // checkDmaHeap=true: WiFi client secrets are written from route handlers
    // dispatched inside wifiManager.process() (main.cpp:620), so the radio is
    // up here. See the WHO PAYS FOR THIS note in storage_manager.h.
    StorageManager::SDLockBlocking sdLock(storage.getSDMutex(), /*checkDmaHeap=*/true);
    if (!sdLock) {
        return "";
    }

    fs::FS* fs = storage.getFilesystem();
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
        Serial.println("[Settings] WARN: SD WiFi secret SSID mismatch");
        return backupFallback();
    }

    const String encoded = doc[WIFI_CLIENT_SD_SECRET_PASSWORD_KEY] | "";
    return encoded.length() > 0 ? encoded : backupFallback();
}

bool removeWifiClientSecretFromSD(StorageManager& storage, size_t slotIndex, const String& ssid) {
    if (!storage.isReady() || !storage.isSDCard()) {
        return false;
    }

    // checkDmaHeap=true: WiFi client secrets are written from route handlers
    // dispatched inside wifiManager.process() (main.cpp:620), so the radio is
    // up here. See the WHO PAYS FOR THIS note in storage_manager.h.
    StorageManager::SDLockBlocking sdLock(storage.getSDMutex(), /*checkDmaHeap=*/true);
    if (!sdLock) {
        return false;
    }

    fs::FS* fs = storage.getFilesystem();
    if (!fs) {
        return false;
    }
    if (!fs->exists(WIFI_CLIENT_SD_SECRET_PATH)) {
        return true;
    }

    WifiClientSdSecretEntry entries[kWifiStaSlotCount];
    JsonDocument existingDoc;
    if (!loadWifiClientSdSecretDocument(fs, existingDoc) || !readWifiClientSdSecretEntries(existingDoc, entries)) {
        return false;
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
        return writeWifiClientSdSecretEntries(fs, entries, kWifiStaSlotCount);
    }
    return true;
}

bool clearWifiClientSecretFromSD(StorageManager& storage) {
    if (!storage.isReady() || !storage.isSDCard()) {
        return false;
    }

    // checkDmaHeap=true: WiFi client secrets are written from route handlers
    // dispatched inside wifiManager.process() (main.cpp:620), so the radio is
    // up here. See the WHO PAYS FOR THIS note in storage_manager.h.
    StorageManager::SDLockBlocking sdLock(storage.getSDMutex(), /*checkDmaHeap=*/true);
    if (!sdLock) {
        return false;
    }

    fs::FS* fs = storage.getFilesystem();
    if (!fs) {
        return false;
    }

    WifiClientSdSecretEntry entries[kWifiStaSlotCount];
    return writeWifiClientSdSecretEntries(fs, entries, kWifiStaSlotCount);
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

bool SettingsManager::resolveWifiCredentialTransaction() {
    if (!wifiCredentialJournalPresent()) {
        return true;
    }

    if (wifiCredentialJournalMode() == WifiCredentialJournalMode::ForgetAll) {
        WifiForgetAllJournal journal;
        if (!readWifiForgetAllJournal(journal)) {
            Serial.println("[Settings] ERROR: WiFi forget transaction journal is invalid");
            return false;
        }
        const bool restoreOld = settings_.hasConfiguredWifiStaSlot() || settings_.wifiClientEnabled;
        String desiredPasswords[kWifiStaSlotCount];
        for (size_t index = 0; index < kWifiStaSlotCount; ++index) {
            if (restoreOld && settings_.wifiStaSlots[index].ssid != journal.oldSsid[index]) {
                Serial.println("[Settings] ERROR: WiFi forget journal does not match selected settings copy");
                return false;
            }
            desiredPasswords[index] = restoreOld ? journal.oldEncodedPassword[index] : String("");
            if (!storeWifiPasswordCandidate(index, desiredPasswords[index])) {
                Serial.println("[Settings] ERROR: Failed to recover WiFi forget password state");
                return false;
            }
        }
        const bool sdResolved = storage_->isReady() &&
                                (!storage_->isSDCard() ||
                                 writeWifiSecretStateFromSettings(*storage_, settings_, desiredPasswords));
        if (!sdResolved) {
            Serial.println("[Settings] WiFi forget NVS recovered; SD recovery remains pending");
            return false;
        }
        if (!clearWifiCredentialJournal()) {
            return false;
        }
        Serial.printf("[Settings] Recovered interrupted WiFi forget transaction (%s)\n",
                      restoreOld ? "rolled back" : "committed");
        return true;
    }

    WifiCredentialJournal journal;
    if (!readWifiCredentialJournal(journal)) {
        Serial.println("[Settings] ERROR: WiFi credential transaction journal is invalid");
        return false;
    }

    const String selectedSsid = settings_.wifiStaSlots[journal.slotIndex].ssid;
    const bool selectedNew = selectedSsid == journal.newSsid;
    const bool selectedOld = selectedSsid == journal.oldSsid;
    if (!selectedNew && !selectedOld) {
        Serial.println("[Settings] WARN: WiFi transaction does not match selected settings copy; deferring recovery");
        return false;
    }
    const String desiredEncoded = selectedNew ? journal.newEncodedPassword : journal.oldEncodedPassword;
    if (!storeWifiPasswordCandidate(journal.slotIndex, desiredEncoded)) {
        Serial.println("[Settings] ERROR: Failed to recover WiFi password transaction");
        return false;
    }

    bool sdResolved = false;
    if (storage_->isReady() && !storage_->isSDCard()) {
        sdResolved = true;
    } else if (storage_->isReady() && storage_->isSDCard()) {
        if (selectedSsid.length() > 0) {
            sdResolved = saveWifiClientSecretToSD(*storage_, journal.slotIndex, selectedSsid, desiredEncoded);
        } else {
            const String removedSsid = selectedNew ? journal.oldSsid : journal.newSsid;
            sdResolved = removeWifiClientSecretFromSD(*storage_, journal.slotIndex, removedSsid);
        }
    }

    if (!sdResolved) {
        Serial.println("[Settings] WiFi password transaction recovered in NVS; SD recovery remains pending");
        return false;
    }
    if (!clearWifiCredentialJournal()) {
        Serial.println("[Settings] WARN: WiFi transaction recovered but journal cleanup is pending");
        return false;
    }

    Serial.printf("[Settings] Recovered interrupted WiFi credential transaction for slot %u\n",
                  static_cast<unsigned>(journal.slotIndex));
    return true;
}

String SettingsManager::loadLastV1AddressFallback() {
    Preferences prefs;
    if (!prefs.begin(kSettingsV1RuntimeNamespace, true)) {
        return "";
    }
    const String address = prefs.isKey(kNvsLastConnectedV1Address)
                               ? sanitizeLastV1AddressValue(prefs.getString(kNvsLastConnectedV1Address, ""))
                               : "";
    prefs.end();
    return address;
}

void SettingsManager::requestLastV1AddressFallbackPersist(const String& addr) {
    const String safeAddr = sanitizeLastV1AddressValue(addr);
    if (safeAddr.length() == 0) {
        return;
    }

    if (loadLastV1AddressFallback() == safeAddr) {
        pendingLastV1AddressFallback_ = "";
        lastV1AddressFallbackPending_ = false;
        lastV1AddressFallbackNextAttemptAtMs_ = 0;
        return;
    }
    if (lastV1AddressFallbackPending_ && pendingLastV1AddressFallback_ == safeAddr) {
        return;
    }

    pendingLastV1AddressFallback_ = safeAddr;
    lastV1AddressFallbackPending_ = true;
    lastV1AddressFallbackNextAttemptAtMs_ = millis() + LAST_V1_FALLBACK_DEBOUNCE_MS;
}

bool SettingsManager::persistLastV1AddressFallbackNow(const String& addr) {
    const String safeAddr = sanitizeLastV1AddressValue(addr);
    if (safeAddr.length() == 0) {
        return false;
    }

    Preferences prefs;
    if (!prefs.begin(kSettingsV1RuntimeNamespace, false)) {
        Serial.println("[Settings] WARN: Failed to open degraded V1 address fallback");
        return false;
    }

    const String existing = prefs.isKey(kNvsLastConnectedV1Address)
                                ? sanitizeLastV1AddressValue(prefs.getString(kNvsLastConnectedV1Address, ""))
                                : "";
    if (existing == safeAddr) {
        prefs.end();
        return true;
    }

    const size_t written = prefs.putString(kNvsLastConnectedV1Address, safeAddr);
    const String verified = sanitizeLastV1AddressValue(prefs.getString(kNvsLastConnectedV1Address, ""));
    prefs.end();
    if (written == 0 || verified != safeAddr) {
        Serial.println("[Settings] WARN: Failed to persist degraded V1 address fallback");
        return false;
    }

    Serial.println("[Settings] Persisted degraded V1 address fallback");
    return true;
}

void SettingsManager::serviceLastV1AddressFallbackPersist(uint32_t nowMs) {
    if (!lastV1AddressFallbackPending_) {
        return;
    }
    if (static_cast<int32_t>(nowMs - lastV1AddressFallbackNextAttemptAtMs_) < 0) {
        return;
    }

    if (!persistLastV1AddressFallbackNow(pendingLastV1AddressFallback_)) {
        lastV1AddressFallbackNextAttemptAtMs_ = nowMs + LAST_V1_FALLBACK_RETRY_MS;
        return;
    }

    pendingLastV1AddressFallback_ = "";
    lastV1AddressFallbackPending_ = false;
    lastV1AddressFallbackNextAttemptAtMs_ = 0;
}

bool SettingsManager::clearLastV1AddressFallback() {
    Preferences prefs;
    if (!prefs.begin(kSettingsV1RuntimeNamespace, false)) {
        Serial.println("[Settings] WARN: Failed to open degraded V1 address fallback for cleanup");
        return false;
    }
    if (!prefs.isKey(kNvsLastConnectedV1Address)) {
        prefs.end();
        pendingLastV1AddressFallback_ = "";
        lastV1AddressFallbackPending_ = false;
        lastV1AddressFallbackNextAttemptAtMs_ = 0;
        return true;
    }

    const bool removed = prefs.remove(kNvsLastConnectedV1Address);
    const bool absent = !prefs.isKey(kNvsLastConnectedV1Address);
    prefs.end();
    if (!removed || !absent) {
        Serial.println("[Settings] WARN: Failed to clear degraded V1 address fallback");
        return false;
    }

    pendingLastV1AddressFallback_ = "";
    lastV1AddressFallbackPending_ = false;
    lastV1AddressFallbackNextAttemptAtMs_ = 0;
    Serial.println("[Settings] Cleared degraded V1 address fallback");
    return true;
}

namespace {

struct SettingsNamespaceState {
    int health = -1;
    uint32_t payloadGeneration = 0;
    uint32_t committedGeneration = 0;
    uint32_t tieBreak = 0;

    bool healthy() const { return health >= 1000; }
    bool isGenerationlessLegacyCopy() const {
        return healthy() && payloadGeneration == 0 && committedGeneration == 0;
    }
    uint32_t generation() const {
        return healthy() && payloadGeneration > 0 && committedGeneration == payloadGeneration
                   ? payloadGeneration
                   : 0;
    }
};

SettingsNamespaceState readSettingsNamespaceState(const char* ns) {
    SettingsNamespaceState state;
    Preferences prefs;
    if (!ns || ns[0] == '\0' || !prefs.begin(ns, true)) {
        return state;
    }

    const int nvsMarker = prefs.getInt(kNvsValid, 0);
    const int settingsVersion = prefs.getInt(kNvsSettingsVer, 0);
    state.health = (nvsMarker > 0 ? 1000 : 0) + (settingsVersion > 0 ? settingsVersion * 10 : 0);
    static constexpr const char* kCriticalKeys[] = {kNvsProxyBle, kNvsProxyName, kNvsBrightness, kNvsAutoPush};
    for (const char* key : kCriticalKeys) {
        state.health += prefs.isKey(key) ? 5 : 0;
    }
    state.payloadGeneration = prefs.getUInt(kNvsSettingsGeneration, 0);
    state.committedGeneration = prefs.getUInt(kNvsCommittedGeneration, 0);

    // Generationless legacy copies can tie on health. Hash persisted content
    // so recovery remains deterministic without preferring a namespace name.
    state.tieBreak = 2166136261u;
    const auto mix = [&](uint32_t value) {
        state.tieBreak ^= value;
        state.tieBreak *= 16777619u;
    };
    mix(static_cast<uint32_t>(nvsMarker));
    mix(static_cast<uint32_t>(settingsVersion));
    mix(prefs.getUInt(kNvsBackupDueRevision, 0));
    mix(prefs.getUChar(kNvsBrightness, 0));
    mix(prefs.getBool(kNvsProxyBle, false) ? 1u : 0u);
    mix(prefs.getBool(kNvsAutoPush, false) ? 1u : 0u);
    const String proxyName = prefs.getString(kNvsProxyName, "");
    for (size_t i = 0; i < proxyName.length(); ++i) {
        mix(static_cast<uint8_t>(proxyName[i]));
    }
    prefs.end();
    return state;
}

bool writeActiveNamespaceCache(const String& active) {
    Preferences meta;
    if (!meta.begin(SETTINGS_NS_META, false)) {
        return false;
    }
    const size_t written = meta.putString(kNvsMetaActive, active);
    const bool saved = written == active.length() && meta.isKey(kNvsMetaActive) &&
                       meta.getString(kNvsMetaActive, "") == active;
    meta.end();
    return saved;
}

} // namespace

int namespaceHealthScore(const char* ns) {
    return readSettingsNamespaceState(ns).health;
}

bool isKnownSettingsNamespace(const String& ns) {
    return ns == SETTINGS_NS_A || ns == SETTINGS_NS_B || ns == SETTINGS_NS_LEGACY;
}

bool finalizeNamespaceGeneration(const char* ns, uint32_t generation) {
    if (!ns || generation == 0) {
        return false;
    }
    Preferences prefs;
    if (!prefs.begin(ns, false)) {
        return false;
    }
    const size_t written = prefs.putUInt(kNvsCommittedGeneration, generation);
    const bool committed = written == sizeof(uint32_t) &&
                           prefs.getUInt(kNvsCommittedGeneration, 0) == generation;
    prefs.end();
    return committed;
}

uint32_t seedLegacyNamespaceGeneration(const String& active, const SettingsNamespaceState& state) {
    if ((active != SETTINGS_NS_A && active != SETTINGS_NS_B) || !state.isGenerationlessLegacyCopy()) {
        return 0;
    }

    constexpr uint32_t generation = 1;
    Preferences prefs;
    if (!prefs.begin(active.c_str(), false)) {
        return 0;
    }
    const bool payloadReady = prefs.putUInt(kNvsSettingsGeneration, generation) == sizeof(uint32_t) &&
                              prefs.getUInt(kNvsSettingsGeneration, 0) == generation;
    const bool committed = payloadReady &&
                           prefs.putUInt(kNvsCommittedGeneration, generation) == sizeof(uint32_t) &&
                           prefs.getUInt(kNvsCommittedGeneration, 0) == generation;
    prefs.end();
    if (!committed) {
        return 0;
    }
    Serial.printf("[Settings] Seeded commit generation %lu in %s\n",
                  static_cast<unsigned long>(generation), active.c_str());
    return generation;
}

String SettingsManager::getActiveNamespace(uint32_t* activeGeneration) {
    if (activeGeneration) {
        *activeGeneration = 0;
    }
    String active = "";
    Preferences meta;
    if (meta.begin(SETTINGS_NS_META, true)) {
        active = meta.getString(kNvsMetaActive, "");
        meta.end();
    }

    const SettingsNamespaceState stateA = readSettingsNamespaceState(SETTINGS_NS_A);
    const SettingsNamespaceState stateB = readSettingsNamespaceState(SETTINGS_NS_B);
    const SettingsNamespaceState stateLegacy = readSettingsNamespaceState(SETTINGS_NS_LEGACY);
    const uint32_t generationA = stateA.generation();
    const uint32_t generationB = stateB.generation();
    String recovered = SETTINGS_NS_LEGACY;

    // Once a committed generation exists, it is the sole transaction
    // authority. The selector is only a cache.
    if (generationA > 0 || generationB > 0) {
        if (generationA > generationB) {
            recovered = SETTINGS_NS_A;
        } else if (generationB > generationA) {
            recovered = SETTINGS_NS_B;
        } else {
            recovered = stateB.tieBreak > stateA.tieBreak ? SETTINGS_NS_B : SETTINGS_NS_A;
        }
        if (activeGeneration) {
            *activeGeneration = generationA > generationB ? generationA : generationB;
        }
    } else {
        const SettingsNamespaceState* recoveredState = &stateLegacy;
        const auto stateFor = [&](const String& ns) -> const SettingsNamespaceState* {
            if (ns == SETTINGS_NS_A) return &stateA;
            if (ns == SETTINGS_NS_B) return &stateB;
            if (ns == SETTINGS_NS_LEGACY) return &stateLegacy;
            return nullptr;
        };
        const SettingsNamespaceState* selectedState = stateFor(active);
        const bool selectedLegacyNamespace = active == SETTINGS_NS_LEGACY && selectedState && selectedState->healthy();
        const bool selectedGenerationlessAb =
            (active == SETTINGS_NS_A || active == SETTINGS_NS_B) && selectedState &&
            selectedState->isGenerationlessLegacyCopy();
        if (selectedLegacyNamespace || selectedGenerationlessAb) {
            // Accept a generationless selector once as legacy input. It never
            // overrides an independently committed A/B copy.
            recovered = active;
            recoveredState = selectedState;
        } else {
            const auto considerLegacyCopy = [&](const char* ns, const SettingsNamespaceState& candidate) {
                if (!candidate.isGenerationlessLegacyCopy()) {
                    return;
                }
                if (candidate.health > recoveredState->health ||
                    (candidate.health == recoveredState->health && candidate.healthy() &&
                     candidate.tieBreak > recoveredState->tieBreak)) {
                    recovered = ns;
                    recoveredState = &candidate;
                }
            };
            considerLegacyCopy(SETTINGS_NS_A, stateA);
            considerLegacyCopy(SETTINGS_NS_B, stateB);
        }
        if (recovered == SETTINGS_NS_A || recovered == SETTINGS_NS_B) {
            const uint32_t seeded = seedLegacyNamespaceGeneration(recovered, *recoveredState);
            if (activeGeneration) {
                *activeGeneration = seeded;
            }
        }
    }

    if (!isKnownSettingsNamespace(active) && active.length() > 0) {
        Serial.printf("[Settings] WARN: Unknown active namespace '%s', recovering\n", active.c_str());
    } else if (isKnownSettingsNamespace(active) && active != recovered) {
        Serial.printf("[Settings] WARN: Cached namespace '%s' is stale or uncommitted; recovering\n",
                      active.c_str());
    }

    if ((recovered == SETTINGS_NS_A || recovered == SETTINGS_NS_B) && recovered != active) {
        if (writeActiveNamespaceCache(recovered)) {
            Serial.printf("[Settings] Recovered active namespace to %s\n", recovered.c_str());
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

bool SettingsManager::writeSettingsToNamespace(const char* ns, uint32_t generation) {
    settings_.ensureWifiStaSlotForLegacyAlias();

    Preferences prefs;
    if (!prefs.begin(ns, false)) {
        Serial.printf("[Settings] ERROR: Failed to open namespace %s for writing\n", ns);
        return false;
    }

    // Clear old keys in this namespace to avoid stale data from previous versions.
    // If clear fails, retaining a previous validity marker could make a partial
    // rewrite look complete.
    if (!prefs.clear()) {
        prefs.end();
        Serial.printf("[Settings] ERROR: Failed to clear namespace %s\n", ns);
        return false;
    }

    struct NvsWriteTracker {
        size_t bytes = 0;
        size_t failures = 0;

        NvsWriteTracker& operator+=(size_t result) {
            bytes += result;
            if (result == 0) {
                ++failures;
            }
            return *this;
        }

        void putString(Preferences& target, const char* key, const String& value) {
            const size_t result = target.putString(key, value);
            bytes += result;
            // Preferences::putString legitimately returns zero for an empty
            // string, so key presence distinguishes success from failure.
            if (result != value.length() || !target.isKey(key)) {
                ++failures;
            }
        }
    } written;

    // The payload generation is staged now. Clearing the namespace removed any
    // old commitGen; a new one is written only after every payload field and
    // the validity marker have been verified.
    written += prefs.putUInt(kNvsSettingsGeneration, generation);

    // Store settings version for migration handling
    written += prefs.putInt(kNvsSettingsVer, SETTINGS_VERSION);
    if (restorePending_) {
        written += prefs.putBool(kNvsRestorePending, true);
    }
    // Transaction watermarks are part of every complete A/B payload. Ordinary
    // settings saves preserve them so a stale filesystem journal can never be
    // mistaken for a new, uncommitted operation after selector/meta loss.
    written += prefs.putLong64(kNvsRestoreCommitWatermark,
                               static_cast<int64_t>(restoreCommitWatermark_));
    written += prefs.putLong64(kNvsProfileDeleteCommitWatermark,
                               static_cast<int64_t>(profileDeleteCommitWatermark_));
    written += prefs.putUInt(kNvsBackupDueRevision, backupDueRevision_);
    written.putString(prefs, kNvsApSsid, settings_.apSSID);
    // Obfuscate passwords before storing
    written.putString(prefs, kNvsApPassword, encodeObfuscatedForStorage(settings_.apPassword));
    // WiFi client (STA) settings - password stored in separate secure namespace
    written += prefs.putBool(kNvsWifiClientEnabled, settings_.wifiClientEnabled);
    for (size_t i = 0; i < kWifiStaSlotCount; ++i) {
        const WifiStaSlot& slot = settings_.wifiStaSlots[i];
        written.putString(prefs, kNvsWifiStaSlotSsid[i], slot.ssid);
        written.putString(prefs, kNvsWifiStaSlotLabel[i], slot.label);
        written += prefs.putUChar(kNvsWifiStaSlotPriority[i], slot.priority);
        written += prefs.putUInt(kNvsWifiStaSlotLastConnected[i], slot.lastConnectedAtSec);
    }
    written += prefs.putBool(kNvsProxyBle, settings_.proxyBLE);
    written.putString(prefs, kNvsProxyName, settings_.proxyName);
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
    written += prefs.putUShort(kNvsColorWifiConnected, settings_.colorWiFiConnected);
    written += prefs.putUShort(kNvsColorBleConnected, settings_.colorBleConnected);
    written += prefs.putUShort(kNvsColorBleDisconnected, settings_.colorBleDisconnected);
    static constexpr const char* kDirectKeys[6] = {
        kNvsLegacyColorBar1, kNvsLegacyColorBar2, kNvsLegacyColorBar3,
        kNvsLegacyColorBar4, kNvsLegacyColorBar5, kNvsLegacyColorBar6,
    };
    for (int barIndex = 0; barIndex < 6; ++barIndex) {
        written += prefs.putUShort(kDirectKeys[barIndex], settings_.colorBars[barIndex]);
    }
    static constexpr const char* kSegmentKeys[8] = {
        kNvsColorBarSeg1, kNvsColorBarSeg2, kNvsColorBarSeg3, kNvsColorBarSeg4,
        kNvsColorBarSeg5, kNvsColorBarSeg6, kNvsColorBarSeg7, kNvsColorBarSeg8,
    };
    uint16_t compatibilitySegments[8];
    DisplayVisualContract::expandSixBarColorsToEight(settings_.colorBars, compatibilitySegments);
    for (int barIndex = 0; barIndex < 8; ++barIndex) {
        written += prefs.putUShort(kSegmentKeys[barIndex], compatibilitySegments[barIndex]);
    }
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
    written.putString(prefs, kNvsSlot0Name, settings_.slot0Name);
    written.putString(prefs, kNvsSlot1Name, settings_.slot1Name);
    written.putString(prefs, kNvsSlot2Name, settings_.slot2Name);
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
    written.putString(prefs, kNvsSlot0Profile, settings_.slot0_default.profileName);
    written += prefs.putInt(kNvsSlot0Mode, settings_.slot0_default.mode);
    written.putString(prefs, kNvsSlot1Profile, settings_.slot1_highway.profileName);
    written += prefs.putInt(kNvsSlot1Mode, settings_.slot1_highway.mode);
    written.putString(prefs, kNvsSlot2Profile, settings_.slot2_comfort.profileName);
    written += prefs.putInt(kNvsSlot2Mode, settings_.slot2_comfort.mode);
    written.putString(prefs, kNvsLastV1Address, settings_.lastV1Address);
    written += prefs.putUChar(kNvsAutoPowerOff, settings_.autoPowerOffMinutes);
    written += prefs.putUChar(kNvsApTimeout, settings_.apTimeoutMinutes);

    // OBD settings
    written += prefs.putBool(kNvsObdEnabled, settings_.obdEnabled);
    written.putString(prefs, kNvsObdAddress, settings_.obdSavedAddress);
    written.putString(prefs, kNvsObdName, settings_.obdSavedName);
    written += prefs.putUChar(kNvsObdAddressType, settings_.obdSavedAddrType);
    written += prefs.putChar(kNvsObdMinRssi, settings_.obdMinRssi);
    written += prefs.putUInt(kNvsCycleObdScanWindow, settings_.obdScanWindowMs);
    written += prefs.putUInt(kNvsCycleObdRetryInt, settings_.obdRetryIntervalMs);
    written += prefs.putUInt(kNvsCycleProxyOpenWindow, settings_.proxyOpenWindowMs);
    written += prefs.putUInt(kNvsCycleV1SettleQuiet, settings_.v1SettleQuietMs);
    written += prefs.putUInt(kNvsCycleV1SettleFallback, settings_.v1SettleFallbackMs);
    written += prefs.putUInt(kNvsCycleTeardownAckTimeout, settings_.cycleTeardownAckTimeoutMs);

    // ALP settings
    written += prefs.putBool(kNvsAlpEnabled, settings_.alpEnabled);
    written += prefs.putUChar(kNvsAlpPersistSec, std::min<uint8_t>(5, settings_.alpAlertPersistSec));
    written += prefs.putBool(kNvsAlpNoV1Laser, settings_.alpDisableV1LaserOnPush);

    // GPS settings
    written += prefs.putBool(kNvsGpsEnabled, settings_.gpsEnabled);
    written += prefs.putUInt(kNvsGpsBaud, settings_.gpsBaud);

    // NVS validity marker - used to detect if NVS was wiped.
    // Written LAST so its presence proves the entire write completed.
    if (written.failures != 0) {
        prefs.end();
        Serial.printf("[Settings] ERROR: %d settings writes failed in %s\n", (int)written.failures, ns);
        return false;
    }
    written += prefs.putInt(kNvsValid, SETTINGS_VERSION);

    // Verify the marker was actually persisted.  If NVS ran out of
    // entries/pages, later keys silently fail and the namespace would
    // appear incomplete on the next boot.
    const int verifyMarker = prefs.getInt(kNvsValid, 0);
    prefs.end();

    if (written.failures != 0 || verifyMarker != SETTINGS_VERSION) {
        Serial.printf("[Settings] ERROR: nvsValid verify failed in %s (expected %d, got %d) — written=%d\n", ns,
                      SETTINGS_VERSION, verifyMarker, (int)written.bytes);
        return false;
    }

    Serial.printf("[Settings] Wrote %d bytes to namespace %s\n", (int)written.bytes, ns);
    return true;
}

bool SettingsManager::persistSettingsAtomically() {
    uint32_t activeGeneration = 0;
    String activeNs = getActiveNamespace(&activeGeneration);
    String stagingNs = getStagingNamespace(activeNs);
    if (activeGeneration == UINT32_MAX) {
        Serial.println("[Settings] ERROR: Settings generation exhausted");
        return false;
    }
    const uint32_t nextGeneration = activeGeneration + 1;

    const auto invalidateStagingMarker = [&]() {
        Preferences staging;
        if (!staging.begin(stagingNs.c_str(), false)) {
            Serial.printf("[Settings] ERROR: Failed to invalidate staging namespace %s\n", stagingNs.c_str());
            return;
        }
        const bool removed = !staging.isKey(kNvsValid) || staging.remove(kNvsValid);
        const bool invalidated = !staging.isKey(kNvsValid);
        staging.end();
        if (!removed || !invalidated) {
            Serial.printf("[Settings] ERROR: Staging namespace %s remains valid\n", stagingNs.c_str());
        }
    };

    if (!writeSettingsToNamespace(stagingNs.c_str(), nextGeneration)) {
        // First attempt failed - try NVS recovery and retry once
        Serial.println("[Settings] First write attempt failed, trying NVS recovery...");
        attemptNvsRecovery(activeNs.c_str());

        if (!writeSettingsToNamespace(stagingNs.c_str(), nextGeneration)) {
            Serial.println("[Settings] ERROR: Failed to write staging settings_ even after recovery");
            return false;
        }
    }

    // Preserve the existing API contract that a cache-write failure fails the
    // save, even though this selector no longer decides which A/B copy boots.
    if (!writeActiveNamespaceCache(stagingNs)) {
        Serial.println("[Settings] ERROR: Failed to update active settings_ namespace");
        invalidateStagingMarker();
        return false;
    }

    if (!finalizeNamespaceGeneration(stagingNs.c_str(), nextGeneration)) {
        Serial.printf("[Settings] ERROR: Failed to finalize generation %lu in %s\n",
                      static_cast<unsigned long>(nextGeneration), stagingNs.c_str());
        writeActiveNamespaceCache(activeNs);
        invalidateStagingMarker();
        return false;
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
    String sdEncoded = loadWifiClientSecretFromSD(*storage_, expectedSsid, index);
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

SettingsPersistResult SettingsManager::setWifiClientEnabled(bool enabled) {
    const V1Settings before = settings_;
    if (settings_.wifiClientEnabled == enabled) {
        return SettingsPersistResult{true, false, false};
    }
    settings_.wifiClientEnabled = enabled;
    if (save()) {
        return SettingsPersistResult{true, true, deferredBackupPending()};
    }
    settings_ = before;
    clearDeferredPersistState();
    return SettingsPersistResult{false, true, false};
}

bool SettingsManager::setWifiStaSlotCredentials(size_t index, const String& ssid, const String& password,
                                                const String& label, uint8_t priority) {
    if (!validWifiStaSlotIndex(index)) {
        return false;
    }
    resolveWifiCredentialTransaction();
    if (wifiCredentialJournalPresent()) {
        return false;
    }

    const V1Settings before = settings_;
    WifiPasswordNvsSnapshot passwordBefore;
    if (!readWifiPasswordNvsSnapshot(index, passwordBefore)) {
        return false;
    }
    const String oldSsid = settings_.wifiStaSlots[index].ssid;
    const String oldEncodedPassword = passwordBefore.slotPresent
                                          ? passwordBefore.slotValue
                                          : (index == 0 && passwordBefore.legacyPresent ? passwordBefore.legacyValue
                                                                                       : String(""));

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

    const String sanitizedPassword = slot.ssid.length() > 0 ? sanitizeWifiClientPasswordValue(password) : String("");
    const String encodedPassword = encodeObfuscatedForStorage(sanitizedPassword);
    const bool sdRequired = storage_->isReady() && storage_->isSDCard();

    const WifiCredentialJournal journal{index, oldSsid, oldEncodedPassword, slot.ssid, encodedPassword};
    if (!writeWifiCredentialJournal(journal)) {
        settings_ = before;
        return false;
    }

    const auto rollbackCredentialStores = [&]() -> bool {
        const bool nvsRestored = restoreWifiPasswordNvsSnapshot(index, passwordBefore);
        bool sdRestored = true;
        if (sdRequired) {
            if (oldSsid.length() > 0) {
                sdRestored = saveWifiClientSecretToSD(*storage_, index, oldSsid, oldEncodedPassword);
            } else {
                sdRestored = removeWifiClientSecretFromSD(*storage_, index, slot.ssid);
            }
        }
        if (!nvsRestored || !sdRestored) {
            Serial.println("[Settings] ERROR: WiFi credential rollback was incomplete");
        }
        const bool journalCleared = nvsRestored && sdRestored && clearWifiCredentialJournal();
        return nvsRestored && sdRestored && journalCleared;
    };

    if (!storeWifiPasswordCandidate(index, encodedPassword)) {
        settings_ = before;
        restoreWifiPasswordNvsSnapshot(index, passwordBefore);
        clearWifiCredentialJournal();
        return false;
    }

    bool sdSaved = true;
    if (sdRequired) {
        sdSaved = slot.ssid.length() > 0
                      ? saveWifiClientSecretToSD(*storage_, index, slot.ssid, encodedPassword)
                      : removeWifiClientSecretFromSD(*storage_, index, oldSsid);
    }
    if (!sdSaved) {
        rollbackCredentialStores();
        settings_ = before;
        return false;
    }

#ifdef UNIT_TEST
    if (wifiCredentialInterruptBeforeSettingsCommit_) {
        wifiCredentialInterruptBeforeSettingsCommit_ = false;
        return false;
    }
#endif

    if (!save()) {
        rollbackCredentialStores();
        settings_ = before;
        return false;
    }

    if (!clearWifiCredentialJournal()) {
        Serial.println("[Settings] WARN: WiFi credential commit journal cleanup deferred to boot recovery");
    }

    Serial.println("[Settings] WiFi client credential transaction committed");
    return true;
}

bool SettingsManager::setWifiClientCredentials(const String& ssid, const String& password) {
    return setWifiStaSlotCredentials(0, ssid, password, settings_.wifiStaSlots[0].label, 0);
}

void SettingsManager::markWifiStaSlotConnected(size_t index, uint32_t connectedAtSec) {
    if (!validWifiStaSlotIndex(index) || !settings_.wifiStaSlots[index].isConfigured()) {
        return;
    }
    settings_.wifiStaSlots[index].lastConnectedAtSec = connectedAtSec;
    settings_.refreshWifiClientAliasFromSlots();
    save();
}

bool SettingsManager::clearWifiStaSlot(size_t index) {
    if (!validWifiStaSlotIndex(index)) {
        return false;
    }
    resolveWifiCredentialTransaction();
    if (wifiCredentialJournalPresent()) {
        return false;
    }
    const V1Settings before = settings_;
    const String removedSsid = settings_.wifiStaSlots[index].ssid;
    WifiPasswordNvsSnapshot passwordBefore;
    if (!readWifiPasswordNvsSnapshot(index, passwordBefore)) {
        return false;
    }
    const String oldEncodedPassword = passwordBefore.slotPresent
                                          ? passwordBefore.slotValue
                                          : (index == 0 && passwordBefore.legacyPresent ? passwordBefore.legacyValue
                                                                                       : String(""));
    settings_.wifiStaSlots[index] = WifiStaSlot();
    settings_.wifiClientEnabled = settings_.hasConfiguredWifiStaSlot();
    settings_.refreshWifiClientAliasFromSlots();

    const bool sdRequired = storage_->isReady() && storage_->isSDCard();
    const WifiCredentialJournal journal{index, removedSsid, oldEncodedPassword, "", ""};
    if (!writeWifiCredentialJournal(journal)) {
        settings_ = before;
        return false;
    }
    if (!storeWifiPasswordCandidate(index, "")) {
        restoreWifiPasswordNvsSnapshot(index, passwordBefore);
        clearWifiCredentialJournal();
        settings_ = before;
        return false;
    }

    const bool sdCleared = !sdRequired ||
                           (settings_.hasConfiguredWifiStaSlot()
                                ? removeWifiClientSecretFromSD(*storage_, index, removedSsid)
                                : clearWifiClientSecretFromSD(*storage_));
    if (!sdCleared || !save()) {
        const bool nvsRestored = restoreWifiPasswordNvsSnapshot(index, passwordBefore);
        bool sdRestored = true;
        if (sdRequired && removedSsid.length() > 0) {
            sdRestored = saveWifiClientSecretToSD(*storage_, index, removedSsid, oldEncodedPassword);
        }
        if (nvsRestored && sdRestored) {
            clearWifiCredentialJournal();
        }
        settings_ = before;
        return false;
    }
    if (!clearWifiCredentialJournal()) {
        Serial.println("[Settings] WARN: WiFi delete journal cleanup deferred to boot recovery");
    }
    return true;
}

bool SettingsManager::clearWifiClientCredentials() {
    resolveWifiCredentialTransaction();
    if (wifiCredentialJournalPresent()) {
        return false;
    }

    const V1Settings before = settings_;
    WifiPasswordNvsSnapshot passwordBefore[kWifiStaSlotCount];
    WifiForgetAllJournal journal;
    for (size_t i = 0; i < kWifiStaSlotCount; ++i) {
        if (!readWifiPasswordNvsSnapshot(i, passwordBefore[i])) {
            return false;
        }
        journal.oldSsid[i] = settings_.wifiStaSlots[i].ssid;
        journal.oldEncodedPassword[i] = passwordBefore[i].slotPresent
                                             ? passwordBefore[i].slotValue
                                             : (i == 0 && passwordBefore[i].legacyPresent
                                                    ? passwordBefore[i].legacyValue
                                                    : String(""));
    }

    bool changed = settings_.wifiClientEnabled || settings_.hasConfiguredWifiStaSlot();
    for (size_t i = 0; i < kWifiStaSlotCount; ++i) {
        changed = changed || passwordBefore[i].slotPresent || (i == 0 && passwordBefore[i].legacyPresent);
    }
    if (!changed) {
        return true;
    }
    if (!writeWifiForgetAllJournal(journal)) {
        return false;
    }

    for (WifiStaSlot& slot : settings_.wifiStaSlots) {
        slot = WifiStaSlot();
    }
    settings_.wifiClientEnabled = false;
    settings_.refreshWifiClientAliasFromSlots();

    bool passwordsCleared = true;
    String emptyPasswords[kWifiStaSlotCount];
    for (size_t i = 0; i < kWifiStaSlotCount; ++i) {
        passwordsCleared = storeWifiPasswordCandidate(i, "") && passwordsCleared;
    }
    const bool sdCleared = passwordsCleared &&
                           writeWifiSecretStateFromSettings(*storage_, settings_, emptyPasswords);

#ifdef UNIT_TEST
    if (passwordsCleared && sdCleared && wifiCredentialInterruptBeforeSettingsCommit_) {
        wifiCredentialInterruptBeforeSettingsCommit_ = false;
        return false;
    }
#endif

    if (!passwordsCleared || !sdCleared || !save()) {
        settings_ = before;
        const bool passwordsRestored = restoreAllWifiPasswordSnapshots(passwordBefore);
        String oldPasswords[kWifiStaSlotCount];
        for (size_t i = 0; i < kWifiStaSlotCount; ++i) {
            oldPasswords[i] = journal.oldEncodedPassword[i];
        }
        const bool sdRestored = writeWifiSecretStateFromSettings(*storage_, before, oldPasswords);
        if (passwordsRestored && sdRestored) {
            clearWifiCredentialJournal();
        }
        return false;
    }
    if (!clearWifiCredentialJournal()) {
        Serial.println("[Settings] WARN: WiFi forget journal cleanup deferred to boot recovery");
    }
    Serial.println("[Settings] WiFi client credentials cleared");
    return true;
}
