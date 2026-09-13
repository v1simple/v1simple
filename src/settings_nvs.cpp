/**
 * Settings NVS persistence, credential obfuscation, and WiFi credentials.
 */

#include "settings_internals.h"
#include "display_visual_contract.h"
#include "json_exact_input.h"
#include "psram_json_document.h"
#include "settings_backup_doc.h"

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
constexpr size_t WIFI_CLIENT_SD_SECRET_MAX_BYTES = 4096;
constexpr size_t WIFI_FORGET_ALL_JOURNAL_MAX_BYTES = 2048;
constexpr size_t MAX_ENCODED_WIFI_PASSWORD_BYTES =
    sizeof("hex:") - 1u + MAX_WIFI_PASSWORD_LEN * 2u;

struct WifiClientSdSecretEntry {
    bool used = false;
    String ssid;
    String encodedPassword;
    uint32_t timestamp = 0;
};

bool wifiClientSdSecretDocumentIsValid(const JsonDocument& doc);

bool copyStringExact(const String& source, String& destination) {
    destination = source;
    return destination.length() == source.length() && destination == source;
}

bool canonicalizeV1AddressBytes(const String& value, char output[18]) {
    size_t begin = 0;
    size_t end = value.length();
    while (begin < end && static_cast<uint8_t>(value[begin]) <= static_cast<uint8_t>(' ')) ++begin;
    while (end > begin && static_cast<uint8_t>(value[end - 1u]) <= static_cast<uint8_t>(' ')) --end;
    if (end - begin != 17u) return false;
    for (size_t index = 0; index < 17u; ++index) {
        char byte = value[begin + index];
        if ((index + 1u) % 3u == 0u) {
            if (byte != ':' && byte != '-') return false;
            output[index] = ':';
            continue;
        }
        if (byte >= 'a' && byte <= 'f') byte = static_cast<char>(byte - 'a' + 'A');
        if (!((byte >= '0' && byte <= '9') || (byte >= 'A' && byte <= 'F'))) return false;
        output[index] = byte;
    }
    output[17] = '\0';
    return true;
}

bool v1AddressMatchesCanonicalBytes(const String& value, const char canonical[18]) {
    char normalized[18] = {};
    return canonicalizeV1AddressBytes(value, normalized) && std::memcmp(normalized, canonical, 17u) == 0;
}

bool nvsSettingsStringFieldsEqual(const V1Settings& lhs, const V1Settings& rhs) {
    if (lhs.apSSID != rhs.apSSID || lhs.apPassword != rhs.apPassword ||
        lhs.wifiClientSSID != rhs.wifiClientSSID || lhs.proxyName != rhs.proxyName ||
        lhs.slot0Name != rhs.slot0Name || lhs.slot1Name != rhs.slot1Name ||
        lhs.slot2Name != rhs.slot2Name || lhs.slot0_default.profileName != rhs.slot0_default.profileName ||
        lhs.slot1_highway.profileName != rhs.slot1_highway.profileName ||
        lhs.slot2_comfort.profileName != rhs.slot2_comfort.profileName ||
        lhs.lastV1Address != rhs.lastV1Address || lhs.obdSavedAddress != rhs.obdSavedAddress ||
        lhs.obdSavedName != rhs.obdSavedName) return false;
    for (size_t index = 0; index < kWifiStaSlotCount; ++index) {
        if (lhs.wifiStaSlots[index].ssid != rhs.wifiStaSlots[index].ssid ||
            lhs.wifiStaSlots[index].label != rhs.wifiStaSlots[index].label) return false;
    }
    return true;
}

bool copySettingsExact(const V1Settings& source, V1Settings& destination) {
    destination = source;
    return nvsSettingsStringFieldsEqual(source, destination);
}

bool copyBoundedSemanticString(const String& source, size_t maxBytes, String& destination) {
    if (source.length() > maxBytes ||
        !ExactJsonInput::validSemanticString(source.c_str(), source.length())) return false;
    return copyStringExact(source, destination);
}

bool refreshWifiClientAliasExact(V1Settings& settings) {
    settings.refreshWifiClientAliasFromSlots();
    const WifiStaSlot* primary = settings.primaryWifiStaSlot();
    return primary ? (settings.wifiClientSSID.length() == primary->ssid.length() &&
                      settings.wifiClientSSID == primary->ssid)
                   : settings.wifiClientSSID.length() == 0;
}

bool readPreferenceStringExact(Preferences& prefs, const char* key, size_t maxBytes, String& output) {
    if (!key || !prefs.isKey(key)) return false;
    const size_t storedBytesWithTerminator = prefs.getStringLength(key);
    if (storedBytesWithTerminator == 0 || storedBytesWithTerminator - 1u > maxBytes) return false;
    String value = prefs.getString(key, "");
    if (value.length() + 1u != storedBytesWithTerminator ||
        std::strlen(value.c_str()) != value.length()) return false;
    output = std::move(value);
    return true;
}

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
            String slotSsid;
            if (exactV1JsonStringChecked(slotObj["ssid"], slotSsid, MAX_WIFI_SSID_LEN) !=
                ExactV1JsonStringStatus::Valid) return "";
            if (expectedSsid.length() > 0 && slotSsid.length() > 0 && slotSsid != expectedSsid) {
                continue;
            }
            if (slotObj[WIFI_STA_SLOT_BACKUP_PASSWORD_KEY].isUnbound()) continue;
            String encoded;
            if (exactV1JsonStringChecked(slotObj[WIFI_STA_SLOT_BACKUP_PASSWORD_KEY], encoded,
                                         sizeof("hex:") - 1u + MAX_WIFI_PASSWORD_LEN * 2u) !=
                ExactV1JsonStringStatus::Valid) return "";
            String decoded = decodeObfuscatedFromStorage(encoded);
            const size_t expectedLength = encoded.startsWith(OBFUSCATION_HEX_PREFIX)
                                              ? (encoded.length() - std::strlen(OBFUSCATION_HEX_PREFIX)) / 2u
                                              : encoded.length();
            if (encoded.length() > 0 && decoded.length() == expectedLength && decoded.length() > 0) {
                return encoded;
            }
            return "";
        }
    }

    if (doc[WIFI_CLIENT_BACKUP_PASSWORD_KEY].isUnbound()) {
        return "";
    }

    String backupSsid;
    if (!doc["wifiClientSSID"].isUnbound() &&
        exactV1JsonStringChecked(doc["wifiClientSSID"], backupSsid, MAX_WIFI_SSID_LEN) !=
            ExactV1JsonStringStatus::Valid) return "";
    if (expectedSsid.length() > 0 && backupSsid.length() > 0 && backupSsid != expectedSsid) {
        return "";
    }

    String encoded;
    if (exactV1JsonStringChecked(doc[WIFI_CLIENT_BACKUP_PASSWORD_KEY], encoded,
                                 sizeof("hex:") - 1u + MAX_WIFI_PASSWORD_LEN * 2u) !=
        ExactV1JsonStringStatus::Valid || encoded.length() == 0) return "";

    // Main-backup credentials are only written for non-empty passwords.  Empty
    // decode means corruption or an unsupported encoding, not an open network.
    String decoded = decodeObfuscatedFromStorage(encoded);
    const size_t expectedLength = encoded.startsWith(OBFUSCATION_HEX_PREFIX)
                                      ? (encoded.length() - std::strlen(OBFUSCATION_HEX_PREFIX)) / 2u
                                      : encoded.length();
    return decoded.length() == expectedLength && decoded.length() > 0 ? encoded : "";
}

bool wifiClientSdSecretTypeMatches(const JsonDocument& doc) {
    return exactV1JsonToken(doc["_type"], WIFI_CLIENT_SD_SECRET_TYPE);
}

bool wifiSecretObjectHasOnlyKeys(JsonObjectConst object, const char* const* keys, size_t keyCount) {
    if (object.size() != keyCount) return false;
    for (JsonPairConst pair : object) {
        const JsonString actual = pair.key();
        bool known = false;
        for (size_t index = 0; index < keyCount; ++index) {
            const size_t expectedLength = std::strlen(keys[index]);
            if (actual.size() == expectedLength &&
                std::memcmp(actual.c_str(), keys[index], expectedLength) == 0) {
                known = true;
                break;
            }
        }
        if (!known) return false;
    }
    return true;
}

bool exactSemanticJsonStringNoCopy(JsonVariantConst value, size_t maxBytes, bool allowEmpty = true) {
    if (!value.is<const char*>()) return false;
    const JsonString text = value.as<JsonString>();
    return text.c_str() && text.size() <= maxBytes && (allowEmpty || text.size() != 0) &&
           std::strlen(text.c_str()) == text.size() &&
           ExactJsonInput::validSemanticString(text.c_str(), text.size());
}

bool obfuscatedPasswordJsonIsValid(JsonVariantConst value, bool allowEmpty) {
    if (!exactSemanticJsonStringNoCopy(value, MAX_ENCODED_WIFI_PASSWORD_BYTES, allowEmpty)) return false;
    const JsonString text = value.as<JsonString>();
    if (text.size() == 0) return allowEmpty;
    const size_t prefixLength = std::strlen(OBFUSCATION_HEX_PREFIX);
    if (text.size() >= prefixLength &&
        std::memcmp(text.c_str(), OBFUSCATION_HEX_PREFIX, prefixLength) == 0) {
        const size_t payloadLength = text.size() - prefixLength;
        if (payloadLength == 0 || (payloadLength & 1u) != 0u ||
            payloadLength / 2u > MAX_WIFI_PASSWORD_LEN) return false;
        for (size_t index = prefixLength; index < text.size(); ++index) {
            if (hexNibble(text.c_str()[index]) < 0) return false;
        }
        return true;
    }
    // Version 1 stored raw XOR bytes. ExactJsonInput has already guaranteed
    // serializer-safe UTF-8/C0 semantics; XOR preserves the byte count.
    return text.size() <= MAX_WIFI_PASSWORD_LEN;
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

            String ssid;
            String encoded;
            if (!exactV1JsonString(entry[WIFI_CLIENT_SD_SECRET_SSID_KEY], ssid, MAX_WIFI_SSID_LEN) ||
                !exactV1JsonString(entry[WIFI_CLIENT_SD_SECRET_PASSWORD_KEY], encoded,
                                   sizeof("hex:") - 1u + MAX_WIFI_PASSWORD_LEN * 2u)) return false;
            if (ssid.length() == 0) {
                continue;
            }

            WifiClientSdSecretEntry& target = entries[static_cast<size_t>(rawIndex)];
            target.used = true;
            target.ssid = std::move(ssid);
            target.encodedPassword = std::move(encoded);
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
    String legacySsid;
    if (!doc[WIFI_CLIENT_SD_SECRET_SSID_KEY].isUnbound() &&
        !exactV1JsonString(doc[WIFI_CLIENT_SD_SECRET_SSID_KEY], legacySsid, MAX_WIFI_SSID_LEN)) return false;
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
                if (!copyStringExact(legacySsid, entries[i].ssid)) return false;
                if (!doc[WIFI_CLIENT_SD_SECRET_PASSWORD_KEY].isUnbound() &&
                    !exactV1JsonString(doc[WIFI_CLIENT_SD_SECRET_PASSWORD_KEY], entries[i].encodedPassword,
                                       sizeof("hex:") - 1u + MAX_WIFI_PASSWORD_LEN * 2u)) return false;
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

enum class WifiClientSdSecretLoadStatus : uint8_t {
    Success,
    NotFound,
    Invalid,
    Unavailable,
};

WifiClientSdSecretLoadStatus loadWifiClientSdSecretDocumentStatus(fs::FS* fs, JsonDocument& doc) {
    doc.clear();
    if (!fs || !fs->exists(WIFI_CLIENT_SD_SECRET_PATH)) {
        return WifiClientSdSecretLoadStatus::NotFound;
    }

    File file = fs->open(WIFI_CLIENT_SD_SECRET_PATH, FILE_READ);
    if (!file) {
        return WifiClientSdSecretLoadStatus::Unavailable;
    }
    const size_t size = file.size();
    if (size == 0 || size > WIFI_CLIENT_SD_SECRET_MAX_BYTES) {
        file.close();
        return WifiClientSdSecretLoadStatus::Invalid;
    }
    PsramJson::Buffer bytes(size);
    if (!bytes) {
        file.close();
        return WifiClientSdSecretLoadStatus::Unavailable;
    }
    if (file.read(bytes.data(), size) != size) {
        file.close();
        return WifiClientSdSecretLoadStatus::Unavailable;
    }
    file.close();
    const ExactJsonInput::Status exact = ExactJsonInput::validate(bytes.data(), size);
    if (exact != ExactJsonInput::Status::Ok) {
        return exact == ExactJsonInput::Status::MemoryUnavailable
                   ? WifiClientSdSecretLoadStatus::Unavailable
                   : WifiClientSdSecretLoadStatus::Invalid;
    }
    DeserializationError err = deserializeJson(doc, bytes.data(), size);
    if (err == DeserializationError::NoMemory || doc.overflowed()) {
        doc.clear();
        return WifiClientSdSecretLoadStatus::Unavailable;
    }
    if (err) {
        Serial.printf("[Settings] WARN: Failed to parse SD WiFi secret: %s\n", err.c_str());
        doc.clear();
        return WifiClientSdSecretLoadStatus::Invalid;
    }

    if (!wifiClientSdSecretDocumentIsValid(doc)) {
        doc.clear();
        return WifiClientSdSecretLoadStatus::Invalid;
    }
    return WifiClientSdSecretLoadStatus::Success;
}

bool loadWifiClientSdSecretDocument(fs::FS* fs, JsonDocument& doc) {
    return loadWifiClientSdSecretDocumentStatus(fs, doc) == WifiClientSdSecretLoadStatus::Success;
}

bool wifiClientSdSecretDocumentIsValid(const JsonDocument& doc) {
    if (!doc.is<JsonObjectConst>() || !wifiClientSdSecretTypeMatches(doc) ||
        !doc["_version"].is<int>()) {
        return false;
    }
    const int version = doc["_version"].as<int>();
    if (version != 1 && version != WIFI_CLIENT_SD_SECRET_VERSION) return false;

    // Version 1 was a single top-level recovery pair.  It remains a valid
    // source for the explicit v1->v2 merge performed by
    // readWifiClientSdSecretEntries(); rejecting it here would strand the only
    // recovery copy before that deterministic upgrade can run.
    if (version == 1) {
        static const char* const REQUIRED[] = {
            "_type", "_version", WIFI_CLIENT_SD_SECRET_SSID_KEY,
            WIFI_CLIENT_SD_SECRET_PASSWORD_KEY, WIFI_CLIENT_SD_SECRET_TIMESTAMP_KEY,
        };
        return wifiSecretObjectHasOnlyKeys(doc.as<JsonObjectConst>(), REQUIRED,
                                           sizeof(REQUIRED) / sizeof(REQUIRED[0])) &&
               exactSemanticJsonStringNoCopy(doc[WIFI_CLIENT_SD_SECRET_SSID_KEY], MAX_WIFI_SSID_LEN,
                                             false) &&
               obfuscatedPasswordJsonIsValid(doc[WIFI_CLIENT_SD_SECRET_PASSWORD_KEY], false) &&
               doc[WIFI_CLIENT_SD_SECRET_TIMESTAMP_KEY].is<uint32_t>();
    }

    static const char* const ROOT_KEYS[] = {
        "_type", "_version", WIFI_CLIENT_SD_SECRETS_KEY, WIFI_CLIENT_SD_SECRET_SSID_KEY,
        WIFI_CLIENT_SD_SECRET_PASSWORD_KEY, WIFI_CLIENT_SD_SECRET_TIMESTAMP_KEY,
    };
    if (!wifiSecretObjectHasOnlyKeys(doc.as<JsonObjectConst>(), ROOT_KEYS,
                                     sizeof(ROOT_KEYS) / sizeof(ROOT_KEYS[0])) ||
        !doc[WIFI_CLIENT_SD_SECRETS_KEY].is<JsonArrayConst>() ||
        doc[WIFI_CLIENT_SD_SECRETS_KEY].size() > kWifiStaSlotCount ||
        !exactSemanticJsonStringNoCopy(doc[WIFI_CLIENT_SD_SECRET_SSID_KEY], MAX_WIFI_SSID_LEN) ||
        !obfuscatedPasswordJsonIsValid(doc[WIFI_CLIENT_SD_SECRET_PASSWORD_KEY], true) ||
        !doc[WIFI_CLIENT_SD_SECRET_TIMESTAMP_KEY].is<uint32_t>()) return false;

    bool seen[kWifiStaSlotCount] = {};
    for (JsonObjectConst entry : doc[WIFI_CLIENT_SD_SECRETS_KEY].as<JsonArrayConst>()) {
        static const char* const ENTRY_KEYS[] = {
            WIFI_CLIENT_SD_SECRET_INDEX_KEY, WIFI_CLIENT_SD_SECRET_SSID_KEY,
            WIFI_CLIENT_SD_SECRET_PASSWORD_KEY, WIFI_CLIENT_SD_SECRET_TIMESTAMP_KEY,
        };
        if (!wifiSecretObjectHasOnlyKeys(entry, ENTRY_KEYS, sizeof(ENTRY_KEYS) / sizeof(ENTRY_KEYS[0])) ||
            !entry[WIFI_CLIENT_SD_SECRET_INDEX_KEY].is<int>() ||
            !exactSemanticJsonStringNoCopy(entry[WIFI_CLIENT_SD_SECRET_SSID_KEY], MAX_WIFI_SSID_LEN,
                                           false) ||
            !obfuscatedPasswordJsonIsValid(entry[WIFI_CLIENT_SD_SECRET_PASSWORD_KEY], true) ||
            !entry[WIFI_CLIENT_SD_SECRET_TIMESTAMP_KEY].is<uint32_t>()) {
            return false;
        }
        const int rawIndex = entry[WIFI_CLIENT_SD_SECRET_INDEX_KEY].as<int>();
        if (rawIndex < 0 || rawIndex >= static_cast<int>(kWifiStaSlotCount) || seen[rawIndex]) {
            return false;
        }
        seen[rawIndex] = true;
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

    PsramJson::Document doc;
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
    if (doc.overflowed() || expectedBytes == 0 || expectedBytes > WIFI_CLIENT_SD_SECRET_MAX_BYTES) {
        file.close();
        fs->remove(WIFI_CLIENT_SD_SECRET_TEMP_PATH);
        return false;
    }
    const size_t writtenBytes = serializeJson(doc, file);
    file.flush();
    file.close();
    if (writtenBytes != expectedBytes) {
        fs->remove(WIFI_CLIENT_SD_SECRET_TEMP_PATH);
        Serial.println("[Settings] WARN: Short write while staging SD WiFi secret");
        return false;
    }

    PsramJson::Document candidate;
    File verifyFile = fs->open(WIFI_CLIENT_SD_SECRET_TEMP_PATH, FILE_READ);
    const size_t candidateSize = verifyFile ? verifyFile.size() : 0;
    PsramJson::Buffer candidateBytes(candidateSize);
    const bool candidateRead = verifyFile && candidateSize > 0 &&
                               candidateSize <= WIFI_CLIENT_SD_SECRET_MAX_BYTES && candidateBytes &&
                               verifyFile.read(candidateBytes.data(), candidateSize) == candidateSize;
    if (verifyFile) {
        verifyFile.close();
    }
    const ExactJsonInput::Status exactStatus = candidateRead
        ? ExactJsonInput::validate(candidateBytes.data(), candidateSize)
        : ExactJsonInput::Status::Invalid;
    const DeserializationError verifyError = exactStatus == ExactJsonInput::Status::Ok
        ? deserializeJson(candidate, candidateBytes.data(), candidateSize)
        : DeserializationError::InvalidInput;
    if (verifyError || candidate.overflowed() || !wifiClientSdSecretDocumentIsValid(candidate)) {
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

    PsramJson::Document backupDoc;
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
    if (snapshot.slotPresent &&
        !readPreferenceStringExact(prefs, passwordKey, MAX_ENCODED_WIFI_PASSWORD_BYTES,
                                   snapshot.slotValue)) {
        prefs.end();
        return false;
    }
    if (slotIndex == 0) {
        snapshot.legacyPresent = prefs.isKey(kNvsWifiPassword);
        if (snapshot.legacyPresent &&
            !readPreferenceStringExact(prefs, kNvsWifiPassword, MAX_ENCODED_WIFI_PASSWORD_BYTES,
                                       snapshot.legacyValue)) {
            prefs.end();
            return false;
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
    if (written != value.length() || !prefs.isKey(key) ||
        prefs.getStringLength(key) != value.length() + 1u) return false;
    if (value.length() == 0) return true;
    String readBack = prefs.getString(key, "");
    return readBack.length() == value.length() && readBack == value;
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
    const bool stringsRead =
        readPreferenceStringExact(prefs, kNvsWifiTxnOldSsid, MAX_WIFI_SSID_LEN, journal.oldSsid) &&
        readPreferenceStringExact(prefs, kNvsWifiTxnOldPass, MAX_ENCODED_WIFI_PASSWORD_BYTES,
                                  journal.oldEncodedPassword) &&
        readPreferenceStringExact(prefs, kNvsWifiTxnNewSsid, MAX_WIFI_SSID_LEN, journal.newSsid) &&
        readPreferenceStringExact(prefs, kNvsWifiTxnNewPass, MAX_ENCODED_WIFI_PASSWORD_BYTES,
                                  journal.newEncodedPassword);
    prefs.end();

    return stringsRead && validWifiStaSlotIndex(journal.slotIndex) &&
           sanitizeWifiClientSsidValue(journal.oldSsid) == journal.oldSsid &&
           sanitizeWifiClientSsidValue(journal.newSsid) == journal.newSsid &&
           (journal.oldEncodedPassword.length() == 0 ||
            decodeObfuscatedFromStorage(journal.oldEncodedPassword).length() > 0) &&
           (journal.newEncodedPassword.length() == 0 ||
            decodeObfuscatedFromStorage(journal.newEncodedPassword).length() > 0);
}

bool writeWifiForgetAllJournal(const WifiForgetAllJournal& journal) {
    PsramJson::Document doc;
    JsonArray slots = doc["slots"].to<JsonArray>();
    for (size_t index = 0; index < kWifiStaSlotCount; ++index) {
        JsonObject slot = slots.add<JsonObject>();
        slot["index"] = index;
        slot["ssid"] = journal.oldSsid[index];
        slot["password"] = journal.oldEncodedPassword[index];
    }
    if (doc.overflowed()) return false;
    const size_t measured = measureJson(doc);
    if (measured == 0 || measured > WIFI_FORGET_ALL_JOURNAL_MAX_BYTES) return false;
    String payload;
    payload.reserve(measured);
    if (serializeJson(doc, payload) != measured || payload.length() != measured) return false;

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
              prefs.getStringLength(kNvsWifiTxnData) == payload.length() + 1u && written;
    if (written && payload.length() > 0) {
        String readBack = prefs.getString(kNvsWifiTxnData, "");
        written = readBack.length() == payload.length() && readBack == payload;
    }
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
    String payload;
    const bool payloadRead = readPreferenceStringExact(
        prefs, kNvsWifiTxnData, WIFI_FORGET_ALL_JOURNAL_MAX_BYTES, payload);
    prefs.end();

    if (!payloadRead || payload.length() == 0 ||
        ExactJsonInput::validate(reinterpret_cast<const uint8_t*>(payload.c_str()), payload.length()) !=
            ExactJsonInput::Status::Ok) return false;
    PsramJson::Document doc;
    if (deserializeJson(doc, payload.c_str(), payload.length()) || doc.overflowed() ||
        !doc.is<JsonObjectConst>() || doc.size() != 1 || !doc["slots"].is<JsonArrayConst>() ||
        doc["slots"].size() != kWifiStaSlotCount) {
        return false;
    }
    bool seen[kWifiStaSlotCount] = {};
    for (JsonObjectConst slot : doc["slots"].as<JsonArrayConst>()) {
        if (slot.size() != 3 || !slot["index"].is<int>() || !slot["ssid"].is<const char*>() ||
            !slot["password"].is<const char*>()) {
            return false;
        }
        const int rawIndex = slot["index"].as<int>();
        if (rawIndex < 0 || rawIndex >= static_cast<int>(kWifiStaSlotCount) || seen[rawIndex]) {
            return false;
        }
        seen[rawIndex] = true;
        const size_t index = static_cast<size_t>(rawIndex);
        if (exactV1JsonStringChecked(slot["ssid"], journal.oldSsid[index], MAX_WIFI_SSID_LEN) !=
                ExactV1JsonStringStatus::Valid ||
            exactV1JsonStringChecked(slot["password"], journal.oldEncodedPassword[index],
                                     MAX_ENCODED_WIFI_PASSWORD_BYTES) !=
                ExactV1JsonStringStatus::Valid ||
            sanitizeWifiClientSsidValue(journal.oldSsid[index]) != journal.oldSsid[index] ||
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
        if (!copyStringExact(settings.wifiStaSlots[index].ssid, entries[index].ssid) ||
            !copyStringExact(encodedPasswords[index], entries[index].encodedPassword)) return false;
        entries[index].timestamp = millis();
    }
    return writeWifiClientSdSecretEntries(fs, entries, kWifiStaSlotCount);
}

} // namespace

WifiClientSecretPresence readWifiClientSecretPresence(fs::FS* fs) {
    WifiClientSecretPresence presence;
    PsramJson::Document doc;
    switch (loadWifiClientSdSecretDocumentStatus(fs, doc)) {
    case WifiClientSdSecretLoadStatus::NotFound:
        presence.status = WifiClientSecretReadStatus::NotFound;
        return presence;
    case WifiClientSdSecretLoadStatus::Invalid:
        presence.status = WifiClientSecretReadStatus::Invalid;
        return presence;
    case WifiClientSdSecretLoadStatus::Unavailable:
        presence.status = WifiClientSecretReadStatus::Unavailable;
        return presence;
    case WifiClientSdSecretLoadStatus::Success:
        break;
    }

    // Presence checks need only one authoritative SSID. Read it directly from
    // the already validated DOM so a password/String allocation failure cannot
    // be downgraded to "no recovery source" and disable a configured client.
    if (doc[WIFI_CLIENT_SD_SECRETS_KEY].is<JsonArrayConst>()) {
        for (JsonObjectConst entry : doc[WIFI_CLIENT_SD_SECRETS_KEY].as<JsonArrayConst>()) {
            String ssid;
            const ExactV1JsonStringStatus status = exactV1JsonStringChecked(
                entry[WIFI_CLIENT_SD_SECRET_SSID_KEY], ssid, MAX_WIFI_SSID_LEN);
            if (status == ExactV1JsonStringStatus::Unavailable) {
                presence.status = WifiClientSecretReadStatus::Unavailable;
                return presence;
            }
            if (status != ExactV1JsonStringStatus::Valid) {
                presence.status = WifiClientSecretReadStatus::Invalid;
                return presence;
            }
            if (ssid.length() == 0) continue;
            presence.ssid = std::move(ssid);
            presence.status = WifiClientSecretReadStatus::Valid;
            return presence;
        }
    }
    if (doc[WIFI_CLIENT_SD_SECRET_SSID_KEY].is<const char*>()) {
        const ExactV1JsonStringStatus status = exactV1JsonStringChecked(
            doc[WIFI_CLIENT_SD_SECRET_SSID_KEY], presence.ssid, MAX_WIFI_SSID_LEN);
        if (status == ExactV1JsonStringStatus::Unavailable) {
            presence.status = WifiClientSecretReadStatus::Unavailable;
            return presence;
        }
        if (status != ExactV1JsonStringStatus::Valid) {
            presence.status = WifiClientSecretReadStatus::Invalid;
            return presence;
        }
    }
    presence.status = WifiClientSecretReadStatus::Valid;
    return presence;
}

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
    PsramJson::Document existingDoc;
    if (fs->exists(WIFI_CLIENT_SD_SECRET_PATH)) {
        if (!loadWifiClientSdSecretDocument(fs, existingDoc) ||
            !wifiClientSdSecretDocumentIsValid(existingDoc) ||
            !readWifiClientSdSecretEntries(existingDoc, entries)) return false;
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

    PsramJson::Document doc;
    const WifiClientSdSecretLoadStatus status = loadWifiClientSdSecretDocumentStatus(fs, doc);
    if (status == WifiClientSdSecretLoadStatus::NotFound) return backupFallback();
    if (status != WifiClientSdSecretLoadStatus::Success) return "";

    WifiClientSdSecretEntry entries[kWifiStaSlotCount];
    if (!readWifiClientSdSecretEntries(doc, entries)) return "";
    if (validWifiStaSlotIndex(expectedSlotIndex) && entries[expectedSlotIndex].used &&
        entries[expectedSlotIndex].ssid == expectedSsid) {
        return std::move(entries[expectedSlotIndex].encodedPassword);
    }
    for (size_t index = 0; index < kWifiStaSlotCount; ++index) {
        if (entries[index].used && entries[index].ssid == expectedSsid) {
            return std::move(entries[index].encodedPassword);
        }
    }
    Serial.println("[Settings] WARN: SD WiFi secret SSID mismatch");
    return "";
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
    PsramJson::Document existingDoc;
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

bool SettingsManager::clearLastV1AddressFallback(const String& addressFilter) {
    const bool clearAll = addressFilter.length() == 0;
    char filter[18] = {};
    if (!clearAll && !canonicalizeV1AddressBytes(addressFilter, filter)) return false;
    const auto matches = [&](const String& value) {
        return clearAll || v1AddressMatchesCanonicalBytes(value, filter);
    };
    Preferences prefs;
    if (!prefs.begin(kSettingsV1RuntimeNamespace, false)) {
        Serial.println("[Settings] WARN: Failed to open degraded V1 address fallback for cleanup");
        return false;
    }
    const bool present = prefs.isKey(kNvsLastConnectedV1Address);
    String stored;
    bool storedMatches = clearAll;
    if (present && !clearAll) {
        const size_t storedBytes = prefs.getStringLength(kNvsLastConnectedV1Address);
        stored = prefs.getString(kNvsLastConnectedV1Address, "");
        // Preferences also returns its empty default on a read/type error.
        // An unreadable key cannot establish that another address owns it.
        char storedCanonical[18] = {};
        if (storedBytes == 0 || stored.length() + 1u != storedBytes ||
            !canonicalizeV1AddressBytes(stored, storedCanonical)) {
            prefs.end();
            return false;
        }
        storedMatches = std::memcmp(storedCanonical, filter, 17u) == 0;
    }
    if (present && storedMatches) {
        const bool removed = prefs.remove(kNvsLastConnectedV1Address);
        const bool absent = !prefs.isKey(kNvsLastConnectedV1Address);
        if (!removed || !absent) {
            prefs.end();
            Serial.println("[Settings] WARN: Failed to clear degraded V1 address fallback");
            return false;
        }
        Serial.println("[Settings] Cleared degraded V1 address fallback");
    }
    prefs.end();

    if (matches(pendingLastV1AddressFallback_)) {
        pendingLastV1AddressFallback_ = "";
        lastV1AddressFallbackPending_ = false;
        lastV1AddressFallbackNextAttemptAtMs_ = 0;
    }
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
    String encodedApPassword;
    if (!encodeObfuscatedForStorage(settings_.apPassword, encodedApPassword)) {
        Serial.println("[Settings] ERROR: AP password encoding memory unavailable");
        return false;
    }
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
    written.putString(prefs, kNvsApPassword, encodedApPassword);
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
    written += prefs.putUChar(kNvsAutoPushProfileSchema, settings_.autoPushProfileSchemaVersion);
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
    const String& expectedSsid = settings_.wifiStaSlots[index].ssid;
    String sdEncoded = loadWifiClientSecretFromSD(*storage_, expectedSsid, index);
    if (sdEncoded.length() == 0) {
        return "";
    }

    String decoded = decodeObfuscatedFromStorage(sdEncoded);
    if (decoded.length() == 0) {
        return "";
    }

    // Heal NVS from SD fallback so future reconnects do not hit SD.
    if (storeWifiClientPasswordObfToNvs(sdEncoded, index)) {
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
    V1Settings before;
    if (!copySettingsExact(settings_, before)) return SettingsPersistResult{};
    if (settings_.wifiClientEnabled == enabled) {
        return SettingsPersistResult{true, false, false};
    }
    settings_.wifiClientEnabled = enabled;
    if (save()) {
        return SettingsPersistResult{true, true, deferredBackupPending()};
    }
    settings_ = std::move(before);
    clearDeferredPersistState();
    return SettingsPersistResult{false, true, false};
}

bool SettingsManager::setWifiStaSlotCredentials(size_t index, const String& ssid, const String& password,
                                                const String& label, uint8_t priority) {
    if (!validWifiStaSlotIndex(index)) {
        Serial.println("[Settings] WARN: Invalid WiFi credential slot index");
        return false;
    }
    resolveWifiCredentialTransaction();
    if (wifiCredentialJournalPresent()) {
        Serial.println("[Settings] WARN: Unresolved WiFi credential transaction");
        return false;
    }

    V1Settings before;
    V1Settings candidate;
    if (!copySettingsExact(settings_, before) || !copySettingsExact(settings_, candidate)) {
        Serial.println("[Settings] WARN: WiFi credential settings snapshot unavailable");
        return false;
    }
    WifiPasswordNvsSnapshot passwordBefore;
    if (!readWifiPasswordNvsSnapshot(index, passwordBefore)) {
        Serial.println("[Settings] WARN: WiFi credential NVS snapshot unavailable");
        return false;
    }
    String preparedSsid;
    String preparedLabel;
    String preparedPassword;
    if (!copyBoundedSemanticString(ssid, MAX_WIFI_SSID_LEN, preparedSsid)) {
        Serial.println("[Settings] WARN: Invalid or unavailable WiFi SSID");
        return false;
    }
    if (!copyBoundedSemanticString(label, MAX_WIFI_STA_LABEL_LEN, preparedLabel)) {
        Serial.println("[Settings] WARN: Invalid or unavailable WiFi slot label");
        return false;
    }
    if (password.length() > MAX_WIFI_PASSWORD_LEN || !copyStringExact(password, preparedPassword)) {
        Serial.println("[Settings] WARN: Invalid or unavailable WiFi password");
        return false;
    }
    preparedLabel.trim();

    WifiStaSlot& slot = candidate.wifiStaSlots[index];
    slot.ssid = std::move(preparedSsid);
    slot.label = std::move(preparedLabel);
    if (slot.label.length() == 0 && slot.ssid.length() > 0) {
        if (index == 0) {
            slot.label = "Saved";
            if (slot.label != "Saved") return false;
        } else if (!copyStringExact(slot.ssid, slot.label)) {
            return false;
        }
    }
    slot.priority = priority;
    if (slot.ssid.length() == 0) {
        slot.label = "";
        slot.lastConnectedAtSec = 0;
    }
    candidate.wifiClientEnabled = candidate.hasConfiguredWifiStaSlot();
    if (!refreshWifiClientAliasExact(candidate)) {
        Serial.println("[Settings] WARN: WiFi credential alias staging unavailable");
        return false;
    }

    if (slot.ssid.length() == 0) preparedPassword = "";
    String encodedPassword;
    if (!encodeObfuscatedForStorage(preparedPassword, encodedPassword)) {
        Serial.println("[Settings] WARN: WiFi credential encoding unavailable");
        return false;
    }
    const bool sdRequired = storage_->isReady() && storage_->isSDCard();

    const String empty;
    const String& oldEncodedPassword = passwordBefore.slotPresent
                                           ? passwordBefore.slotValue
                                           : (index == 0 && passwordBefore.legacyPresent
                                                  ? passwordBefore.legacyValue : empty);
    WifiCredentialJournal journal;
    journal.slotIndex = index;
    if (!copyStringExact(before.wifiStaSlots[index].ssid, journal.oldSsid) ||
        !copyStringExact(oldEncodedPassword, journal.oldEncodedPassword) ||
        !copyStringExact(slot.ssid, journal.newSsid) ||
        !copyStringExact(encodedPassword, journal.newEncodedPassword)) {
        Serial.println("[Settings] WARN: WiFi credential journal staging unavailable");
        return false;
    }
    if (!writeWifiCredentialJournal(journal)) {
        Serial.println("[Settings] WARN: WiFi credential journal write failed");
        return false;
    }

    settings_ = std::move(candidate);
    WifiStaSlot& activeSlot = settings_.wifiStaSlots[index];

    const auto rollbackCredentialStores = [&]() -> bool {
        const bool nvsRestored = restoreWifiPasswordNvsSnapshot(index, passwordBefore);
        bool sdRestored = true;
        if (sdRequired) {
            if (journal.oldSsid.length() > 0) {
                sdRestored = saveWifiClientSecretToSD(*storage_, index, journal.oldSsid,
                                                      journal.oldEncodedPassword);
            } else {
                sdRestored = removeWifiClientSecretFromSD(*storage_, index, journal.newSsid);
            }
        }
        if (!nvsRestored || !sdRestored) {
            Serial.println("[Settings] ERROR: WiFi credential rollback was incomplete");
        }
        const bool journalCleared = nvsRestored && sdRestored && clearWifiCredentialJournal();
        return nvsRestored && sdRestored && journalCleared;
    };

    if (!storeWifiPasswordCandidate(index, encodedPassword)) {
        settings_ = std::move(before);
        restoreWifiPasswordNvsSnapshot(index, passwordBefore);
        clearWifiCredentialJournal();
        return false;
    }

    bool sdSaved = true;
    if (sdRequired) {
        sdSaved = activeSlot.ssid.length() > 0
                      ? saveWifiClientSecretToSD(*storage_, index, activeSlot.ssid, encodedPassword)
                      : removeWifiClientSecretFromSD(*storage_, index, journal.oldSsid);
    }
    if (!sdSaved) {
        rollbackCredentialStores();
        settings_ = std::move(before);
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
        settings_ = std::move(before);
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
    V1Settings before;
    V1Settings candidate;
    if (!copySettingsExact(settings_, before) || !copySettingsExact(settings_, candidate)) return false;
    WifiPasswordNvsSnapshot passwordBefore;
    if (!readWifiPasswordNvsSnapshot(index, passwordBefore)) {
        return false;
    }
    const String empty;
    const String& oldEncodedPassword = passwordBefore.slotPresent
                                           ? passwordBefore.slotValue
                                           : (index == 0 && passwordBefore.legacyPresent
                                                  ? passwordBefore.legacyValue : empty);
    candidate.wifiStaSlots[index] = WifiStaSlot();
    candidate.wifiClientEnabled = candidate.hasConfiguredWifiStaSlot();
    if (!refreshWifiClientAliasExact(candidate)) return false;

    const bool sdRequired = storage_->isReady() && storage_->isSDCard();
    WifiCredentialJournal journal;
    journal.slotIndex = index;
    if (!copyStringExact(before.wifiStaSlots[index].ssid, journal.oldSsid) ||
        !copyStringExact(oldEncodedPassword, journal.oldEncodedPassword)) return false;
    if (!writeWifiCredentialJournal(journal)) {
        return false;
    }
    settings_ = std::move(candidate);
    if (!storeWifiPasswordCandidate(index, "")) {
        restoreWifiPasswordNvsSnapshot(index, passwordBefore);
        clearWifiCredentialJournal();
        settings_ = std::move(before);
        return false;
    }

    const bool sdCleared = !sdRequired ||
                           (settings_.hasConfiguredWifiStaSlot()
                                ? removeWifiClientSecretFromSD(*storage_, index, journal.oldSsid)
                                : clearWifiClientSecretFromSD(*storage_));
    if (!sdCleared || !save()) {
        const bool nvsRestored = restoreWifiPasswordNvsSnapshot(index, passwordBefore);
        bool sdRestored = true;
        if (sdRequired && journal.oldSsid.length() > 0) {
            sdRestored = saveWifiClientSecretToSD(*storage_, index, journal.oldSsid,
                                                  journal.oldEncodedPassword);
        }
        if (nvsRestored && sdRestored) {
            clearWifiCredentialJournal();
        }
        settings_ = std::move(before);
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

    V1Settings before;
    V1Settings candidate;
    if (!copySettingsExact(settings_, before) || !copySettingsExact(settings_, candidate)) return false;
    WifiPasswordNvsSnapshot passwordBefore[kWifiStaSlotCount];
    WifiForgetAllJournal journal;
    const String empty;
    for (size_t i = 0; i < kWifiStaSlotCount; ++i) {
        if (!readWifiPasswordNvsSnapshot(i, passwordBefore[i])) {
            return false;
        }
        const String& encoded = passwordBefore[i].slotPresent
                                    ? passwordBefore[i].slotValue
                                    : (i == 0 && passwordBefore[i].legacyPresent
                                           ? passwordBefore[i].legacyValue : empty);
        if (!copyStringExact(settings_.wifiStaSlots[i].ssid, journal.oldSsid[i]) ||
            !copyStringExact(encoded, journal.oldEncodedPassword[i])) return false;
    }

    bool changed = settings_.wifiClientEnabled || settings_.hasConfiguredWifiStaSlot();
    for (size_t i = 0; i < kWifiStaSlotCount; ++i) {
        changed = changed || passwordBefore[i].slotPresent || (i == 0 && passwordBefore[i].legacyPresent);
    }
    if (!changed) {
        return true;
    }
    String oldPasswords[kWifiStaSlotCount];
    for (size_t i = 0; i < kWifiStaSlotCount; ++i) {
        if (!copyStringExact(journal.oldEncodedPassword[i], oldPasswords[i])) return false;
    }
    for (WifiStaSlot& slot : candidate.wifiStaSlots) {
        slot = WifiStaSlot();
    }
    candidate.wifiClientEnabled = false;
    if (!refreshWifiClientAliasExact(candidate)) return false;
    if (!writeWifiForgetAllJournal(journal)) return false;

    settings_ = std::move(candidate);

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
        settings_ = std::move(before);
        const bool passwordsRestored = restoreAllWifiPasswordSnapshots(passwordBefore);
        const bool sdRestored = writeWifiSecretStateFromSettings(*storage_, settings_, oldPasswords);
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
