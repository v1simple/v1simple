/**
 * Settings backup-document parsing and application.
 */

#include <cctype>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <utility>

#include "backup_payload_builder.h"
#include "display_visual_contract.h"
#include "json_exact_input.h"
#include "psram_json_document.h"
#include "settings_backup_doc.h"
#include "settings_sanitize.h"
#include "v1_settings_json.h"

namespace {

bool exactStringCopy(const String& source, String& destination) {
    destination = source;
    return destination.length() == source.length() && destination == source;
}

bool settingsStringFieldsEqual(const V1Settings& lhs, const V1Settings& rhs) {
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

bool copySettingsChecked(const V1Settings& source, V1Settings& destination) {
    destination = source;
    return settingsStringFieldsEqual(source, destination);
}

bool profileStringFieldsEqual(const V1Profile& lhs, const V1Profile& rhs) {
    return lhs.name == rhs.name && lhs.description == rhs.description;
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

bool captureRestoreCredentialSnapshot(StorageManager& storage, RestoreCredentialSnapshot& snapshot);
bool restoreCredentialSnapshot(StorageManager& storage, const RestoreCredentialSnapshot& snapshot);

} // namespace

bool loadBestBackupDocument(fs::FS* fs, JsonDocument& outDoc, const char** outPath, bool verboseErrors,
                            BackupDocumentLoadStatus* outStatus) {
    outDoc.clear();
    if (outStatus) *outStatus = BackupDocumentLoadStatus::NotFound;
    if (outPath) {
        *outPath = nullptr;
    }
    if (!fs) {
        return false;
    }

    int bestScore = -1;
    uint32_t bestContentCrc = 0;
    const char* bestPath = nullptr;
    {
        // Candidates may be as large as the shared 128 KiB document limit. Keep the
        // sole scan document in PSRAM and destroy it before loading the winner;
        // never retain a second full String copy in internal SRAM.
        PsramJson::Document candidateDoc;
        for (size_t i = 0; i < SETTINGS_BACKUP_CANDIDATES_COUNT; ++i) {
            const char* candidate = SETTINGS_BACKUP_CANDIDATES[i];
            if (!fs->exists(candidate)) {
                continue;
            }

            candidateDoc.clear();
            BackupDocumentLoadStatus candidateStatus = BackupDocumentLoadStatus::Invalid;
            if (!parseBackupFile(fs, candidate, candidateDoc, verboseErrors, &candidateStatus)) {
                if (candidateStatus == BackupDocumentLoadStatus::MemoryUnavailable ||
                    candidateStatus == BackupDocumentLoadStatus::IoError) {
                    outDoc.clear();
                    if (outStatus) *outStatus = candidateStatus;
                    if (verboseErrors) {
                        Serial.printf("[Settings] Backup candidate unavailable; aborting selection: %s\n", candidate);
                    }
                    return false;
                }
                if (verboseErrors) {
                    Serial.printf("[Settings] WARN: Ignoring invalid backup candidate: %s\n", candidate);
                }
                continue;
            }

            const int score = backupCandidateScore(candidateDoc);
            if (score > bestScore) {
                bestScore = score;
                bestPath = candidate;
                bestContentCrc = BackupPayloadBuilder::computeBackupCrc32(candidateDoc);
            }
        }
    }

    if (bestScore < 0 || !bestPath) {
        if (outStatus) *outStatus = BackupDocumentLoadStatus::Invalid;
        return false;
    }

    // Re-read the selected candidate after releasing the scan allocation. A
    // score + canonical content-CRC check makes this two-pass, no-copy choice
    // fail closed if the file changes between reads.
    outDoc.clear();
    BackupDocumentLoadStatus selectedStatus = BackupDocumentLoadStatus::Invalid;
    if (!parseBackupFile(fs, bestPath, outDoc, verboseErrors, &selectedStatus) || outDoc.overflowed() ||
        backupCandidateScore(outDoc) != bestScore ||
        BackupPayloadBuilder::computeBackupCrc32(outDoc) != bestContentCrc) {
        const bool selectedOverflowed = outDoc.overflowed();
        outDoc.clear();
        if (outStatus) {
            *outStatus = selectedOverflowed ? BackupDocumentLoadStatus::MemoryUnavailable
                                            : (selectedStatus == BackupDocumentLoadStatus::Success
                                                   ? BackupDocumentLoadStatus::Invalid
                                                   : selectedStatus);
        }
        if (verboseErrors) {
            Serial.printf("[Settings] Selected backup changed or exhausted PSRAM while loading: %s\n", bestPath);
        }
        return false;
    }

    if (outPath) {
        *outPath = bestPath;
    }
    if (outStatus) *outStatus = BackupDocumentLoadStatus::Success;
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
        const JsonString raw = value.as<JsonString>();
        if (!raw || std::strlen(raw.c_str()) != raw.size()) return false;
        size_t first = 0;
        size_t last = raw.size();
        while (first < last && std::isspace(static_cast<unsigned char>(raw.c_str()[first]))) ++first;
        while (last > first && std::isspace(static_cast<unsigned char>(raw.c_str()[last - 1u]))) --last;
        const auto equalsIgnoreCase = [&](const char* expected) {
            const size_t expectedLength = std::strlen(expected);
            if (last - first != expectedLength) return false;
            for (size_t index = 0; index < expectedLength; ++index) {
                if (std::tolower(static_cast<unsigned char>(raw.c_str()[first + index])) !=
                    std::tolower(static_cast<unsigned char>(expected[index]))) return false;
            }
            return true;
        };
        if (equalsIgnoreCase("1") || equalsIgnoreCase("true") || equalsIgnoreCase("on") ||
            equalsIgnoreCase("yes")) {
            out = true;
            return true;
        }
        if (equalsIgnoreCase("0") || equalsIgnoreCase("false") || equalsIgnoreCase("off") ||
            equalsIgnoreCase("no")) {
            out = false;
            return true;
        }
    }
    return false;
}

namespace {

constexpr size_t kMaxEncodedWifiPasswordBytes =
    sizeof("hex:") - 1u + MAX_WIFI_PASSWORD_LEN * 2u;
constexpr size_t kMaxWifiSecretSnapshotBytes = 4096u;

bool readBackupPreferenceStringExact(Preferences& prefs, const char* key,
                                     size_t maxBytes, String& output) {
    if (!key || !prefs.isKey(key)) return false;
    const size_t storedBytesWithTerminator = prefs.getStringLength(key);
    if (storedBytesWithTerminator == 0 || storedBytesWithTerminator - 1u > maxBytes) return false;
    String value = prefs.getString(key, "");
    if (value.length() + 1u != storedBytesWithTerminator ||
        std::strlen(value.c_str()) != value.length()) return false;
    output = std::move(value);
    return true;
}

bool decodeObfuscatedChecked(const String& encoded, String& decoded) {
    if (encoded.length() == 0) return false;
    size_t expectedLength = encoded.length();
    if (encoded.startsWith(OBFUSCATION_HEX_PREFIX)) {
        const size_t prefixLength = std::strlen(OBFUSCATION_HEX_PREFIX);
        if (encoded.length() <= prefixLength || ((encoded.length() - prefixLength) & 1u) != 0u) return false;
        expectedLength = (encoded.length() - prefixLength) / 2u;
    }
    decoded = decodeObfuscatedFromStorage(encoded);
    return decoded.length() == expectedLength && decoded.length() > 0 &&
           decoded.length() <= MAX_WIFI_PASSWORD_LEN &&
           std::memchr(decoded.c_str(), '\0', decoded.length()) == nullptr;
}

bool exactBackupString(JsonVariantConst value, String& output, size_t maxBytes) {
    return exactV1JsonStringChecked(value, output, maxBytes) == ExactV1JsonStringStatus::Valid;
}

bool isTrimmedString(const String& value) {
    if (value.length() == 0) return true;
    return !std::isspace(static_cast<unsigned char>(value[0])) &&
           !std::isspace(static_cast<unsigned char>(value[value.length() - 1u]));
}

struct PreparedWifiStaSlot {
    bool present = false;
    bool configured = false;
    WifiStaSlot slot;
    bool hasPasswordObf = false;
    String passwordObf;
};

struct PreparedNetworkFields {
    bool hasApPassword = false;
    String apPassword;
    bool hasApSsid = false;
    String apSsid;
    bool hasWifiClientEnabled = false;
    bool wifiClientEnabled = false;
    bool hasSlotDocument = false;
    PreparedWifiStaSlot slots[kWifiStaSlotCount];
    bool hasLegacySsid = false;
    String legacySsid;
    bool hasWifiClientPasswordObf = false;
    String wifiClientPasswordObf;
    bool hasLegacyStationPassword = false;
    String legacyStationPasswordObf;
    bool hasProxyName = false;
    String proxyName;
    bool hasLastV1Address = false;
    String lastV1Address;
    bool mutatesCredentials = false;
};

bool prepareNetworkFields(const JsonDocument& doc, BackupRestoreScope scope, PreparedNetworkFields& prepared) {
    if (!doc["apPassword"].isUnbound() && !doc["apPassword"].isNull()) {
        String encoded;
        String decoded;
        if (!exactBackupString(doc["apPassword"], encoded, kMaxEncodedWifiPasswordBytes) ||
            !decodeObfuscatedChecked(encoded, decoded) || decoded.length() < MIN_AP_PASSWORD_LEN) return false;
        prepared.hasApPassword = true;
        prepared.apPassword = std::move(decoded);
    }
    if (!doc["apSSID"].isUnbound() && !doc["apSSID"].isNull()) {
        String value;
        if (!exactBackupString(doc["apSSID"], value, MAX_WIFI_SSID_LEN)) return false;
        if (value.length() == 0) value = "V1-Simple";
        if (value.length() == 0) return false;
        prepared.hasApSsid = true;
        prepared.apSsid = std::move(value);
    }
    if (!doc["wifiClientEnabled"].isUnbound() && !doc["wifiClientEnabled"].isNull()) {
        if (!parseBoolVariant(doc["wifiClientEnabled"], prepared.wifiClientEnabled)) return false;
        prepared.hasWifiClientEnabled = true;
    }

    if (!doc["wifiStaSlots"].isUnbound() && !doc["wifiStaSlots"].isNull()) {
        if (!doc["wifiStaSlots"].is<JsonArrayConst>()) return false;
        prepared.hasSlotDocument = true;
        bool seen[kWifiStaSlotCount] = {};
        for (JsonVariantConst value : doc["wifiStaSlots"].as<JsonArrayConst>()) {
            if (!value.is<JsonObjectConst>()) return false;
            const JsonObjectConst source = value.as<JsonObjectConst>();
            if (!source["index"].is<int>() || !source["ssid"].is<const char*>()) return false;
            const int rawIndex = source["index"].as<int>();
            if (rawIndex < 0 || rawIndex >= static_cast<int>(kWifiStaSlotCount) || seen[rawIndex]) return false;
            seen[rawIndex] = true;
            PreparedWifiStaSlot& target = prepared.slots[static_cast<size_t>(rawIndex)];
            target.present = true;
            if (!exactBackupString(source["ssid"], target.slot.ssid, MAX_WIFI_SSID_LEN)) return false;
            target.configured = target.slot.ssid.length() > 0;

            if (!source["label"].isUnbound() && !source["label"].isNull()) {
                if (!exactBackupString(source["label"], target.slot.label, MAX_WIFI_STA_LABEL_LEN) ||
                    !isTrimmedString(target.slot.label)) return false;
            }
            if (target.configured && target.slot.label.length() == 0) {
                if (rawIndex == 0) {
                    target.slot.label = "Saved";
                } else if (!exactStringCopy(target.slot.ssid, target.slot.label)) {
                    return false;
                }
            }
            target.slot.priority = source["priority"].is<int>()
                                       ? clampU8(source["priority"].as<int>(), 0, 255)
                                       : static_cast<uint8_t>(rawIndex);
            if (!source["priority"].isUnbound() && !source["priority"].isNull() &&
                !source["priority"].is<int>()) return false;
            if (source["lastConnectedAtSec"].is<uint32_t>()) {
                // v21 retains this historical field name. Treat its value as
                // an opaque logical order token; older uptime-derived values
                // preserve their numeric order until new connections advance
                // above them.
                target.slot.lastConnectedAtSec = source["lastConnectedAtSec"].as<uint32_t>();
            } else if (source["lastConnectedAtSec"].is<int>()) {
                target.slot.lastConnectedAtSec =
                    static_cast<uint32_t>(std::max(0, source["lastConnectedAtSec"].as<int>()));
            } else if (!source["lastConnectedAtSec"].isUnbound() && !source["lastConnectedAtSec"].isNull()) {
                return false;
            }
            if (!source["passwordObf"].isUnbound() && !source["passwordObf"].isNull()) {
                String decoded;
                if (!exactBackupString(source["passwordObf"], target.passwordObf,
                                       kMaxEncodedWifiPasswordBytes) ||
                    !decodeObfuscatedChecked(target.passwordObf, decoded)) return false;
                target.hasPasswordObf = true;
            }
        }
    }

    const auto prepareLegacySsid = [&](const char* key, String& value) {
        if (doc[key].isUnbound() || doc[key].isNull()) return true;
        return exactBackupString(doc[key], value, MAX_WIFI_SSID_LEN);
    };
    String wifiClientSsid;
    String stationSsid;
    if (!prepareLegacySsid("wifiClientSSID", wifiClientSsid) ||
        !prepareLegacySsid("stationSSID", stationSsid)) return false;
    if (wifiClientSsid.length() > 0 || stationSsid.length() > 0) {
        prepared.hasLegacySsid = true;
        prepared.legacySsid = wifiClientSsid.length() > 0 ? std::move(wifiClientSsid) : std::move(stationSsid);
    }
    if (!doc["wifiClientPasswordObf"].isUnbound() && !doc["wifiClientPasswordObf"].isNull()) {
        String decoded;
        if (!prepared.hasLegacySsid ||
            !exactBackupString(doc["wifiClientPasswordObf"], prepared.wifiClientPasswordObf,
                               kMaxEncodedWifiPasswordBytes) ||
            !decodeObfuscatedChecked(prepared.wifiClientPasswordObf, decoded)) return false;
        prepared.hasWifiClientPasswordObf = true;
    }
    if (!doc["stationPassword"].isUnbound() && !doc["stationPassword"].isNull()) {
        String plain;
        if (!prepared.hasLegacySsid ||
            !exactBackupString(doc["stationPassword"], plain, MAX_WIFI_PASSWORD_LEN) || plain.length() == 0 ||
            !encodeObfuscatedForStorage(plain, prepared.legacyStationPasswordObf)) return false;
        prepared.hasLegacyStationPassword = true;
    }
    if (prepared.hasSlotDocument && prepared.hasLegacySsid &&
        (prepared.hasWifiClientPasswordObf || prepared.hasLegacyStationPassword)) {
        int primary = -1;
        for (size_t index = 0; index < kWifiStaSlotCount; ++index) {
            if (!prepared.slots[index].configured) continue;
            if (primary < 0 || prepared.slots[index].slot.priority < prepared.slots[primary].slot.priority ||
                (prepared.slots[index].slot.priority == prepared.slots[primary].slot.priority &&
                 prepared.slots[index].slot.lastConnectedAtSec >
                     prepared.slots[primary].slot.lastConnectedAtSec)) {
                primary = static_cast<int>(index);
            }
        }
        if (primary < 0 || prepared.slots[primary].slot.ssid != prepared.legacySsid) return false;
    }

    if (scope == BackupRestoreScope::Full) {
        if (!doc["proxyName"].isUnbound() && !doc["proxyName"].isNull()) {
            if (!exactBackupString(doc["proxyName"], prepared.proxyName, MAX_PROXY_NAME_LEN)) return false;
            if (prepared.proxyName.length() == 0) prepared.proxyName = "V1-Proxy";
            if (prepared.proxyName.length() == 0) return false;
            prepared.hasProxyName = true;
        }
        if (!doc["lastV1Address"].isUnbound() && !doc["lastV1Address"].isNull()) {
            if (!exactBackupString(doc["lastV1Address"], prepared.lastV1Address, 32)) return false;
            String sanitized = sanitizeLastV1AddressValue(prepared.lastV1Address);
            if (sanitized.length() != prepared.lastV1Address.length() || sanitized != prepared.lastV1Address) return false;
            prepared.hasLastV1Address = true;
        }
    }
    prepared.mutatesCredentials = prepared.hasSlotDocument || prepared.hasLegacySsid ||
                                   prepared.hasWifiClientPasswordObf || prepared.hasLegacyStationPassword;
    return true;
}

} // namespace

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
        if (!exactStringCopy(settings.wifiStaSlots[i].ssid, snapshot[i].ssid)) {
            prefs.end();
            return false;
        }
        snapshot[i].passwordObf = "";
        if (snapshot[i].ssid.length() > 0 && prefs.isKey(kNvsWifiStaSlotPassword[i])) {
            String encoded;
            String decoded;
            if (!readBackupPreferenceStringExact(prefs, kNvsWifiStaSlotPassword[i],
                                                 kMaxEncodedWifiPasswordBytes, encoded) ||
                !decodeObfuscatedChecked(encoded, decoded)) {
                prefs.end();
                return false;
            }
            snapshot[i].passwordObf = std::move(encoded);
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

bool applyPreparedWifiStaSlots(PreparedNetworkFields& prepared, V1Settings& settings, StorageManager& storage,
                               bool clearSdSecret) {
    if (!prepared.hasSlotDocument) return false;
    StoredSlotPasswordSnapshot storedPasswords[kWifiStaSlotCount];
    if (!snapshotWifiStaSlotPasswords(settings, storedPasswords)) {
        return false;
    }

    const String* finalPasswords[kWifiStaSlotCount] = {};
    for (size_t index = 0; index < kWifiStaSlotCount; ++index) {
        PreparedWifiStaSlot& incoming = prepared.slots[index];
        if (!incoming.configured) continue;
        if (incoming.hasPasswordObf) {
            finalPasswords[index] = &incoming.passwordObf;
            continue;
        }
        for (size_t oldIndex = 0; oldIndex < kWifiStaSlotCount; ++oldIndex) {
            if (storedPasswords[oldIndex].passwordObf.length() > 0 &&
                storedPasswords[oldIndex].ssid == incoming.slot.ssid) {
                finalPasswords[index] = &storedPasswords[oldIndex].passwordObf;
                break;
            }
        }
    }

    if (!clearWifiStaSlotPasswordsForRestore(storage, clearSdSecret)) {
        return false;
    }

    for (size_t i = 0; i < kWifiStaSlotCount; ++i) {
        settings.wifiStaSlots[i] = WifiStaSlot();
    }

    bool restoredAny = false;
    for (size_t index = 0; index < kWifiStaSlotCount; ++index) {
        PreparedWifiStaSlot& incoming = prepared.slots[index];
        if (!incoming.configured) continue;
        settings.wifiStaSlots[index] = std::move(incoming.slot);
        if (finalPasswords[index] && !storeWifiClientPasswordObfToNvs(*finalPasswords[index], index)) return false;
        restoredAny = true;
    }

    if (prepared.hasWifiClientPasswordObf &&
        !storeWifiClientPasswordObfToNvs(prepared.wifiClientPasswordObf, 0)) return false;
    if (prepared.hasLegacyStationPassword &&
        !storeWifiClientPasswordObfToNvs(prepared.legacyStationPasswordObf, 0)) return false;

    if (restoredAny && !settings.wifiClientEnabled && !prepared.hasWifiClientEnabled) {
        settings.wifiClientEnabled = true;
    }
    settings.refreshWifiClientAliasFromSlots();

    if (clearSdSecret) {
        for (size_t index = 0; index < kWifiStaSlotCount; ++index) {
            const String* encoded = finalPasswords[index];
            if (index == 0 && prepared.hasWifiClientPasswordObf) encoded = &prepared.wifiClientPasswordObf;
            if (index == 0 && prepared.hasLegacyStationPassword) encoded = &prepared.legacyStationPasswordObf;
            if (settings.wifiStaSlots[index].isConfigured() && encoded &&
                !saveWifiClientSecretToSD(storage, index, settings.wifiStaSlots[index].ssid, *encoded)) return false;
        }
    }
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

bool applyPreparedNetworkFields(const JsonDocument& doc, PreparedNetworkFields& prepared, V1Settings& settings,
                                StorageManager& storage, BackupRestoreScope scope, bool clearSdSecret) {
    const bool synchronizeSecrets = clearSdSecret && prepared.mutatesCredentials;
    if (synchronizeSecrets && !storage.isReady()) {
        return false;
    }
    // Healthy LittleFS fallback still restores NVS credentials; only their
    // optional SD mirror is absent. Errors accessing selected SD remain failures.
    const bool synchronizeSdSecrets = synchronizeSecrets && storage.isSDCard();
    if (prepared.hasApPassword) settings.apPassword = std::move(prepared.apPassword);
    if (prepared.hasApSsid) settings.apSSID = std::move(prepared.apSsid);
    if (prepared.hasWifiClientEnabled) settings.wifiClientEnabled = prepared.wifiClientEnabled;
    bool restoredSlots = false;
    if (prepared.hasSlotDocument) {
        if (!applyPreparedWifiStaSlots(prepared, settings, storage, synchronizeSdSecrets)) {
            return false;
        }
        restoredSlots = true;
    }
    if (!restoredSlots && prepared.hasLegacySsid) {
        if (!clearWifiStaSlotPasswordsForRestore(storage, synchronizeSdSecrets)) {
            return false;
        }
        for (WifiStaSlot& slot : settings.wifiStaSlots) slot = WifiStaSlot();
        settings.wifiStaSlots[0].ssid = std::move(prepared.legacySsid);
        settings.wifiStaSlots[0].label = "Saved";
        settings.wifiStaSlots[0].priority = 0;
        if (prepared.hasWifiClientPasswordObf &&
            !storeWifiClientPasswordObfToNvs(prepared.wifiClientPasswordObf, 0)) return false;
        if (prepared.hasLegacyStationPassword &&
            !storeWifiClientPasswordObfToNvs(prepared.legacyStationPasswordObf, 0)) return false;
        const String* encoded = prepared.hasLegacyStationPassword ? &prepared.legacyStationPasswordObf
                               : prepared.hasWifiClientPasswordObf ? &prepared.wifiClientPasswordObf
                                                                  : nullptr;
        if (synchronizeSdSecrets && encoded &&
            !saveWifiClientSecretToSD(storage, 0, settings.wifiStaSlots[0].ssid, *encoded)) return false;
    }
    if (!settings.wifiClientEnabled && settings.hasConfiguredWifiStaSlot() && !prepared.hasWifiClientEnabled)
        settings.wifiClientEnabled = true;
    settings.refreshWifiClientAliasFromSlots();

    if (scope == BackupRestoreScope::Full) {
        restoreBackupBool(doc, "proxyBLE", settings.proxyBLE);
        if (prepared.hasProxyName) settings.proxyName = std::move(prepared.proxyName);
        if (prepared.hasLastV1Address) settings.lastV1Address = std::move(prepared.lastV1Address);
        if (doc["autoPowerOffMinutes"].is<int>())
            settings.autoPowerOffMinutes = clampU8(doc["autoPowerOffMinutes"], 0, 60);
        if (doc["apTimeoutMinutes"].is<int>())
            settings.apTimeoutMinutes = clampApTimeoutValue(doc["apTimeoutMinutes"]);
    }

    return true;
}

bool applyBackupNetworkFields(const JsonDocument& doc, V1Settings& settings, StorageManager& storage,
                              BackupRestoreScope scope, bool clearSdSecret) {
    PreparedNetworkFields prepared;
    if (!prepareNetworkFields(doc, scope, prepared)) return false;
    V1Settings before;
    RestoreCredentialSnapshot credentialsBefore;
    if (!copySettingsChecked(settings, before) ||
        (prepared.mutatesCredentials && !captureRestoreCredentialSnapshot(storage, credentialsBefore))) return false;
    if (applyPreparedNetworkFields(doc, prepared, settings, storage, scope, clearSdSecret)) return true;
    settings = std::move(before);
    if (prepared.mutatesCredentials && !restoreCredentialSnapshot(storage, credentialsBefore)) {
        Serial.println("[Settings] ERROR: Network fallback credential rollback incomplete");
    }
    return false;
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
        {"colorMuted", &V1Settings::colorMuted, 0x4A49, false},
        {"colorPersisted", &V1Settings::colorPersisted, 0x4208, false},
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

struct PreparedProfileSlotFields {
    bool hasProfileName[3] = {};
    String profileNames[3];
    bool hasDisplayName[3] = {};
    String displayNames[3];
};

void applyBackupProfileSlotFields(const JsonDocument& doc, V1Settings& settings, BackupRestoreScope scope,
                                  PreparedProfileSlotFields& prepared) {
    restoreBackupBool(doc, "autoPushEnabled", settings.autoPushEnabled);
    if (doc["autoPushProfileSchemaVersion"].is<int>()) {
        const int version = doc["autoPushProfileSchemaVersion"].as<int>();
        settings.autoPushProfileSchemaVersion =
            (version == V1_PROFILE_PREVIOUS_SCHEMA_VERSION || version == V1_PROFILE_SCHEMA_VERSION)
                ? V1_PROFILE_SCHEMA_VERSION
                : 0;
    } else if (doc["profiles"].is<JsonArrayConst>() || !doc["slot0Mode"].isUnbound() ||
               !doc["slot0DarkMode"].isUnbound() || !doc["slot0Volume"].isUnbound()) {
        // A pre-v2 document carries detector commands in slot fields. Never
        // inherit a destination device's v2 marker across that restore.
        settings.autoPushProfileSchemaVersion = 0;
    }
    if (doc["activeSlot"].is<int>()) settings.activeSlot = std::max(0, std::min(doc["activeSlot"].as<int>(), 2));
    AutoPushSlot* slots[] = {&settings.slot0_default, &settings.slot1_highway, &settings.slot2_comfort};
    for (int i = 0; i < 3; ++i) {
        char profileKey[24], modeKey[16];
        std::snprintf(profileKey, sizeof(profileKey), "slot%dProfileName", i);
        std::snprintf(modeKey, sizeof(modeKey), "slot%dMode", i);
        if (prepared.hasProfileName[i]) slots[i]->profileName = std::move(prepared.profileNames[i]);
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
        std::snprintf(key, sizeof(key), "slot%dName", i);
        if (prepared.hasDisplayName[i]) *names[i] = std::move(prepared.displayNames[i]);
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

bool prepareProfileSlotStrings(const JsonDocument& doc, PreparedProfileSlotFields& prepared) {
    for (int slot = 0; slot < 3; ++slot) {
        char key[24];
        std::snprintf(key, sizeof(key), "slot%dProfileName", slot);
        if (!doc[key].isUnbound()) {
            if (exactV1JsonStringChecked(doc[key], prepared.profileNames[slot], MAX_PROFILE_NAME_LEN) !=
                ExactV1JsonStringStatus::Valid) return false;
            if (prepared.profileNames[slot].length() > 0) {
                String canonical;
                if (canonicalizeProfileName(prepared.profileNames[slot], canonical) !=
                        ProfileNameStatus::Valid || canonical != prepared.profileNames[slot]) return false;
            }
            prepared.hasProfileName[slot] = true;
        }
        std::snprintf(key, sizeof(key), "slot%dName", slot);
        if (!doc[key].isUnbound()) {
            if (exactV1JsonStringChecked(doc[key], prepared.displayNames[slot], MAX_SLOT_NAME_LEN) !=
                ExactV1JsonStringStatus::Valid) return false;
            prepared.hasDisplayName[slot] = true;
        }
    }
    return true;
}

bool applyBackupProfileSlotFields(const JsonDocument& doc, V1Settings& settings, BackupRestoreScope scope) {
    PreparedProfileSlotFields prepared;
    if (!prepareProfileSlotStrings(doc, prepared)) return false;
    applyBackupProfileSlotFields(doc, settings, scope, prepared);
    return true;
}

namespace {

struct PreparedObdFields {
    bool hasSavedName = false;
    String savedName;
    bool hasSavedAddress = false;
    String savedAddress;
};

// A full restore keeps the pre-restore settings and every validated external
// field alive until the NVS selector commits or rollback completes. Keeping
// that bounded transaction state on the loopTask stack made the fresh-NVS
// restore path retain more than a kilobyte before entering profile storage.
// Allocate it as one nothrow candidate instead; publication and rollback still
// use the same validated objects and fail before any mutation when unavailable.
struct RestoreApplyStaging {
    V1Settings settingsBefore;
    std::vector<V1Profile> incomingProfiles;
    std::vector<V1Profile> profilesBefore;
    PreparedProfileSlotFields preparedSlots;
    PreparedNetworkFields preparedNetwork;
    PreparedObdFields preparedObd;
    RestoreCredentialSnapshot credentialsBefore;
};

bool prepareObdFields(const JsonDocument& doc, PreparedObdFields& prepared) {
    if (!doc["obdSavedName"].isUnbound() && !doc["obdSavedName"].isNull()) {
        if (!exactBackupString(doc["obdSavedName"], prepared.savedName, 32) ||
            !isTrimmedString(prepared.savedName)) return false;
        prepared.hasSavedName = true;
    }
    if (!doc["obdSavedAddress"].isUnbound() && !doc["obdSavedAddress"].isNull()) {
        if (!exactBackupString(doc["obdSavedAddress"], prepared.savedAddress, 17) ||
            !isValidBleAddress(prepared.savedAddress)) return false;
        prepared.hasSavedAddress = true;
    }
    return true;
}

void applyPreparedObdFields(const JsonDocument& doc, V1Settings& settings, BackupRestoreScope scope,
                            PreparedObdFields& prepared) {
    restoreBackupBool(doc, "obdEnabled", settings.obdEnabled);
    if (prepared.hasSavedName) settings.obdSavedName = std::move(prepared.savedName);
    if (scope != BackupRestoreScope::Full) return;
    if (prepared.hasSavedAddress) settings.obdSavedAddress = std::move(prepared.savedAddress);
    if (doc["obdSavedAddrType"].is<int>()) settings.obdSavedAddrType = clampU8(doc["obdSavedAddrType"], 0, 1);
    if (doc["obdMinRssi"].is<int>()) settings.obdMinRssi = static_cast<int8_t>(std::max(-100, std::min(doc["obdMinRssi"].as<int>(), -40)));
    if (doc["obdScanWindowMs"].is<int>()) settings.obdScanWindowMs = clampConnectionCycleObdScanWindowMsValue(doc["obdScanWindowMs"]);
    if (doc["obdRetryIntervalMs"].is<int>()) settings.obdRetryIntervalMs = clampConnectionCycleObdRetryIntervalMsValue(doc["obdRetryIntervalMs"]);
    if (doc["proxyOpenWindowMs"].is<int>()) settings.proxyOpenWindowMs = clampConnectionCycleProxyOpenWindowMsValue(doc["proxyOpenWindowMs"]);
    if (doc["v1SettleQuietMs"].is<int>()) settings.v1SettleQuietMs = clampConnectionCycleV1SettleQuietMsValue(doc["v1SettleQuietMs"]);
    if (doc["v1SettleFallbackMs"].is<int>()) settings.v1SettleFallbackMs = clampConnectionCycleV1SettleFallbackMsValue(doc["v1SettleFallbackMs"]);
    if (doc["cycleTeardownAckTimeoutMs"].is<int>()) settings.cycleTeardownAckTimeoutMs = clampConnectionCycleTeardownAckTimeoutMsValue(doc["cycleTeardownAckTimeoutMs"]);
}

} // namespace

void applyBackupObdFields(const JsonDocument& doc, V1Settings& settings, BackupRestoreScope scope) {
    PreparedObdFields prepared;
    if (!prepareObdFields(doc, prepared)) return;
    applyPreparedObdFields(doc, settings, scope, prepared);
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

bool applyBackupCriticalFieldsAtomically(const JsonDocument& doc, V1Settings& settings,
                                         StorageManager& storage,
                                         bool (*persist)(void* ctx), void* persistCtx) {
    PreparedNetworkFields network;
    PreparedProfileSlotFields slots;
    PreparedObdFields obd;
    V1Settings before;
    RestoreCredentialSnapshot credentialsBefore;
    if (!prepareNetworkFields(doc, BackupRestoreScope::CriticalRecovery, network) ||
        !prepareProfileSlotStrings(doc, slots) || !prepareObdFields(doc, obd) ||
        !copySettingsChecked(settings, before) ||
        (network.mutatesCredentials && !captureRestoreCredentialSnapshot(storage, credentialsBefore))) {
        return false;
    }
    if (!applyPreparedNetworkFields(doc, network, settings, storage,
                                    BackupRestoreScope::CriticalRecovery, false)) {
        settings = std::move(before);
        if (network.mutatesCredentials) restoreCredentialSnapshot(storage, credentialsBefore);
        return false;
    }
    applyBackupDisplayFields(doc, settings, BackupRestoreScope::CriticalRecovery);
    applyBackupAudioFields(doc, settings, BackupRestoreScope::CriticalRecovery);
    applyBackupProfileSlotFields(doc, settings, BackupRestoreScope::CriticalRecovery, slots);
    applyPreparedObdFields(doc, settings, BackupRestoreScope::CriticalRecovery, obd);
    applyBackupAlpAndGpsFields(doc, settings);
    healBackupRestoreConflicts(settings, "recovered");
    if (persist && !persist(persistCtx)) {
        settings = std::move(before);
        if (network.mutatesCredentials && !restoreCredentialSnapshot(storage, credentialsBefore)) {
            Serial.println("[Settings] ERROR: Critical recovery credential rollback failed");
        }
        return false;
    }
    return true;
}

bool applyBackupWifiClientHealingAtomically(const JsonDocument& doc, V1Settings& settings,
                                            StorageManager& storage, bool& recovered,
                                            bool (*persist)(void* ctx), void* persistCtx) {
    recovered = false;
    PreparedNetworkFields network;
    if (!prepareNetworkFields(doc, BackupRestoreScope::CriticalRecovery, network)) return false;

    bool hasConfiguredSource = network.hasLegacySsid;
    for (size_t index = 0; index < kWifiStaSlotCount; ++index) {
        hasConfiguredSource = hasConfiguredSource || network.slots[index].configured;
    }
    if (!hasConfiguredSource) return true;

    // This healthy-NVS repair path owns only station state. AP credentials and
    // unrelated fields remain authoritative in NVS.
    network.hasApPassword = false;
    network.hasApSsid = false;
    network.hasProxyName = false;
    network.hasLastV1Address = false;
    network.hasWifiClientEnabled = true;
    network.wifiClientEnabled = true;

    V1Settings before;
    RestoreCredentialSnapshot credentialsBefore;
    if (!copySettingsChecked(settings, before) ||
        !captureRestoreCredentialSnapshot(storage, credentialsBefore)) return false;
    if (!applyPreparedNetworkFields(doc, network, settings, storage,
                                    BackupRestoreScope::CriticalRecovery, false) ||
        (persist && !persist(persistCtx))) {
        settings = std::move(before);
        if (!restoreCredentialSnapshot(storage, credentialsBefore)) {
            Serial.println("[Settings] ERROR: WiFi healing credential rollback failed");
        }
        return false;
    }
    recovered = true;
    return true;
}

// Profile entries processed between watchdog feeds inside the profile restore
// loop.  Every entry costs a filesystem write, so feeding per batch bounds the
// gap between feeds without putting a feed on the per-field path.
static constexpr int kProfileRestoreWatchdogFeedInterval = 4;

namespace {

bool jsonKeyEquals(JsonString actual, const char* expected) {
    const size_t expectedLength = std::strlen(expected);
    return actual.size() == expectedLength &&
           std::memcmp(actual.c_str(), expected, expectedLength) == 0;
}

bool jsonObjectHasOnlyKeys(JsonObjectConst object, const char* const* allowed, size_t allowedCount) {
    for (JsonPairConst pair : object) {
        bool known = false;
        for (size_t index = 0; index < allowedCount; ++index) {
            if (jsonKeyEquals(pair.key(), allowed[index])) {
                known = true;
                break;
            }
        }
        if (!known) return false;
    }
    return true;
}

bool optionalExactBool(const JsonDocument& doc, const char* key) {
    return doc[key].isUnbound() || doc[key].is<bool>();
}

bool optionalExactIntRange(const JsonDocument& doc, const char* key, int minimum, int maximum) {
    return doc[key].isUnbound() ||
           (doc[key].is<int>() && doc[key].as<int>() >= minimum && doc[key].as<int>() <= maximum);
}

bool optionalExactUint32Range(const JsonDocument& doc, const char* key, uint32_t minimum, uint32_t maximum) {
    return doc[key].isUnbound() ||
           (doc[key].is<uint32_t>() && doc[key].as<uint32_t>() >= minimum &&
            doc[key].as<uint32_t>() <= maximum);
}

bool optionalExactString(const JsonDocument& doc, const char* key) {
    return doc[key].isUnbound() || doc[key].is<const char*>();
}

bool currentJsonStringIsValid(JsonVariantConst value, size_t maxBytes, bool allowEmpty = true) {
    if (!value.is<const char*>()) return false;
    const JsonString text = value.as<JsonString>();
    return (allowEmpty || text.size() != 0) && text.size() <= maxBytes &&
           std::strlen(text.c_str()) == text.size() &&
           ExactJsonInput::validSemanticString(text.c_str(), text.size());
}

bool currentJsonStringIsTrimmed(JsonVariantConst value) {
    const JsonString text = value.as<JsonString>();
    return text.size() == 0 ||
           (!std::isspace(static_cast<unsigned char>(text.c_str()[0])) &&
            !std::isspace(static_cast<unsigned char>(text.c_str()[text.size() - 1u])));
}

bool currentJsonBleAddressIsValid(JsonVariantConst value) {
    const JsonString text = value.as<JsonString>();
    if (text.size() == 0) return true;
    if (text.size() != 17) return false;
    for (size_t index = 0; index < text.size(); ++index) {
        const unsigned char byte = static_cast<unsigned char>(text.c_str()[index]);
        if ((index + 1u) % 3u == 0u) {
            if (byte != ':') return false;
        } else if (!std::isxdigit(byte)) {
            return false;
        }
    }
    return true;
}

bool currentJsonProfileNameIsCanonical(JsonVariantConst value, bool allowEmpty = false) {
    if (!currentJsonStringIsValid(value, MAX_PROFILE_NAME_LEN, allowEmpty)) return false;
    const JsonString text = value.as<JsonString>();
    if (text.size() == 0) return allowEmpty;
    if (text.c_str()[0] == '.' || text.c_str()[0] == '_' || text.c_str()[0] == ' ' ||
        text.c_str()[text.size() - 1u] == ' ') return false;
    for (size_t index = 0; index < text.size(); ++index) {
        const unsigned char byte = static_cast<unsigned char>(text.c_str()[index]);
        if (byte == '/' || byte == '\\' || byte == ':' || byte == '*' || byte == '?' ||
            byte == '"' || byte == '<' || byte == '>' || byte == '|' ||
            (byte == '.' && index + 1u < text.size() && text.c_str()[index + 1u] == '.')) {
            return false;
        }
    }
    return true;
}

bool currentJsonProfileNamesCollide(JsonVariantConst lhs, JsonVariantConst rhs) {
    const JsonString left = lhs.as<JsonString>();
    const JsonString right = rhs.as<JsonString>();
    if (left.size() != right.size()) return false;
    for (size_t index = 0; index < left.size(); ++index) {
        unsigned char leftByte = static_cast<unsigned char>(left.c_str()[index]);
        unsigned char rightByte = static_cast<unsigned char>(right.c_str()[index]);
        if (leftByte >= 'A' && leftByte <= 'Z') leftByte = static_cast<unsigned char>(leftByte - 'A' + 'a');
        if (rightByte >= 'A' && rightByte <= 'Z') rightByte = static_cast<unsigned char>(rightByte - 'A' + 'a');
        if (leftByte != rightByte) return false;
    }
    return true;
}

bool currentJsonRawSettingsAreValid(JsonVariantConst value) {
    if (!value.is<JsonArrayConst>() || value.size() != V1SettingsJson::kSettingsByteCount) return false;
    for (JsonVariantConst byte : value.as<JsonArrayConst>()) {
        if (!byte.is<int>() || byte.as<int>() < 0 || byte.as<int>() > 255) return false;
    }
    return true;
}

bool currentJsonDetectorIsValid(JsonObjectConst source) {
    if (source.isNull() || source.size() != 6 || !source["userSettings"].is<const char*>() ||
        !source["mode"].is<JsonObjectConst>() || !source["display"].is<const char*>() ||
        !source["volume"].is<JsonObjectConst>() || !source["bluetoothLed"].is<const char*>() ||
        !source["customFrequencies"].is<JsonObjectConst>()) return false;
    if (!exactV1JsonToken(source["userSettings"], "value") &&
        !exactV1JsonToken(source["userSettings"], "unchanged")) return false;

    const JsonObjectConst mode = source["mode"].as<JsonObjectConst>();
    if (exactV1JsonToken(mode["policy"], "value")) {
        if (mode.size() != 2 || !mode["value"].is<int>() || mode["value"].as<int>() < 1 ||
            mode["value"].as<int>() > 3) return false;
    } else if (!exactV1JsonToken(mode["policy"], "unchanged") || mode.size() != 1) {
        return false;
    }
    if (!exactV1JsonToken(source["display"], "on") &&
        !exactV1JsonToken(source["display"], "off") &&
        !exactV1JsonToken(source["display"], "unchanged")) return false;

    const JsonObjectConst volume = source["volume"].as<JsonObjectConst>();
    const bool temporary = exactV1JsonToken(volume["policy"], "temporary");
    const bool saved = exactV1JsonToken(volume["policy"], "saved");
    if (temporary || saved) {
        if (volume.size() != 5 || !volume["main"].is<int>() || !volume["muted"].is<int>() ||
            volume["main"].as<int>() < 0 || volume["main"].as<int>() > 9 ||
            volume["muted"].as<int>() < 0 || volume["muted"].as<int>() > 9 ||
            (!exactV1JsonToken(volume["feedback"], "none") &&
             !exactV1JsonToken(volume["feedback"], "changed_only") &&
             !exactV1JsonToken(volume["feedback"], "always")) ||
            (!exactV1JsonToken(volume["disconnect"], "restore_saved") &&
             !exactV1JsonToken(volume["disconnect"], "keep_current")) ||
            (saved && !exactV1JsonToken(volume["disconnect"], "restore_saved"))) return false;
    } else if (!exactV1JsonToken(volume["policy"], "unchanged") || volume.size() != 1) {
        return false;
    }
    if (!exactV1JsonToken(source["bluetoothLed"], "on") &&
        !exactV1JsonToken(source["bluetoothLed"], "off") &&
        !exactV1JsonToken(source["bluetoothLed"], "unchanged")) return false;

    const JsonObjectConst custom = source["customFrequencies"].as<JsonObjectConst>();
    if (exactV1JsonToken(custom["policy"], "unchanged")) return custom.size() == 1;
    if (!exactV1JsonToken(custom["policy"], "value") || custom.size() != 2 ||
        !custom["definitions"].is<JsonArrayConst>()) return false;
    const JsonArrayConst definitions = custom["definitions"].as<JsonArrayConst>();
    if (definitions.size() == 0 || definitions.size() > 64) return false;
    uint8_t expectedIndex = 0;
    for (JsonVariantConst value : definitions) {
        if (!value.is<JsonObjectConst>()) return false;
        const JsonObjectConst definition = value.as<JsonObjectConst>();
        if (definition.size() != 3 || !definition["index"].is<int>() ||
            definition["index"].as<int>() != expectedIndex++ || !definition["lowerMHz"].is<int>() ||
            !definition["upperMHz"].is<int>() || definition["lowerMHz"].as<int>() < 0 ||
            definition["lowerMHz"].as<int>() > 65535 || definition["upperMHz"].as<int>() < 0 ||
            definition["upperMHz"].as<int>() > 65535) return false;
        const int lower = definition["lowerMHz"].as<int>();
        const int upper = definition["upperMHz"].as<int>();
        const bool unused = lower == 0 && upper == 0;
        if ((lower == 0) != (upper == 0) || (!unused && lower >= upper)) return false;
    }
    return true;
}

bool currentJsonEncodedPasswordIsValid(JsonVariantConst value) {
    if (!value.is<const char*>()) return false;
    const JsonString encoded = value.as<JsonString>();
    static constexpr char prefix[] = "hex:";
    if (!encoded.c_str() || std::strlen(encoded.c_str()) != encoded.size() ||
        encoded.size() < sizeof(prefix) - 1u ||
        std::memcmp(encoded.c_str(), prefix, sizeof(prefix) - 1u) != 0) return false;
    const size_t hexLength = encoded.size() - (sizeof(prefix) - 1u);
    if ((hexLength & 1u) != 0u || hexLength / 2u < MIN_AP_PASSWORD_LEN ||
        hexLength / 2u > MAX_AP_PASSWORD_LEN) return false;
    char decodedBytes[MAX_AP_PASSWORD_LEN + 1u] = {};
    const size_t decodedLength = hexLength / 2u;
    const size_t keyLength = std::strlen(XOR_KEY);
    for (size_t decodedIndex = 0; decodedIndex < decodedLength; ++decodedIndex) {
        const char high = encoded.c_str()[sizeof(prefix) - 1u + decodedIndex * 2u];
        const char low = encoded.c_str()[sizeof(prefix) + decodedIndex * 2u];
        const auto canonicalNibble = [](char byte) -> int {
            if (byte >= '0' && byte <= '9') return byte - '0';
            if (byte >= 'A' && byte <= 'F') return byte - 'A' + 10;
            return -1;
        };
        const int highNibble = canonicalNibble(high);
        const int lowNibble = canonicalNibble(low);
        if (highNibble < 0 || lowNibble < 0) return false;
        const uint8_t obfuscated = static_cast<uint8_t>((highNibble << 4) | lowNibble);
        const uint8_t decoded = obfuscated ^ static_cast<uint8_t>(XOR_KEY[decodedIndex % keyLength]);
        decodedBytes[decodedIndex] = static_cast<char>(decoded);
    }
    decodedBytes[decodedLength] = '\0';
    // Candidate ranking must enforce the same semantic string contract as the
    // current writer. Otherwise a CRC-valid primary can suppress a valid
    // previous backup, apply bytes that later cannot be serialized, and leave
    // the device unable to produce its next authoritative backup.
    return ExactJsonInput::validSemanticString(decodedBytes, decodedLength);
}

bool validateCurrentBackupSchema(const JsonDocument& doc) {
    static constexpr const char* kTopLevelKeys[] = {
        "_type", "_version", "_timestamp", "timestamp", "_crc32",
        "apSSID", "apPassword", "wifiClientEnabled", "wifiClientSSID", "wifiStaSlots",
        "proxyBLE", "proxyName", "lastV1Address", "autoPowerOffMinutes", "apTimeoutMinutes",
        "obdEnabled", "obdSavedAddress", "obdSavedName", "obdSavedAddrType", "obdMinRssi",
        "obdScanWindowMs", "obdRetryIntervalMs", "proxyOpenWindowMs", "v1SettleQuietMs",
        "v1SettleFallbackMs", "cycleTeardownAckTimeoutMs", "alpEnabled", "alpAlertPersistSec",
        "alpDisableV1LaserOnPush", "gpsEnabled", "gpsBaud", "brightness",
        "colorBogey", "colorFrequency", "colorArrowFront", "colorArrowSide", "colorArrowRear",
        "colorBandL", "colorBandKa", "colorBandK", "colorBandX", "colorBandPhoto",
        "colorWiFiIcon", "colorWiFiConnected", "colorBleConnected", "colorBleDisconnected",
        "colorBar1", "colorBar2", "colorBar3", "colorBar4", "colorBar5", "colorBar6",
        "colorBarS1", "colorBarS2", "colorBarS3", "colorBarS4", "colorBarS5", "colorBarS6",
        "colorBarS7", "colorBarS8", "colorMuted", "colorPersisted", "colorVolumeMain",
        "colorVolumeMute", "colorRssiV1", "colorRssiProxy", "colorObd", "colorAlpConnected",
        "colorAlpDli", "colorAlpLidActive", "colorAlpAlert", "freqUseBandColor",
        "hideWifiIcon", "hideProfileIndicator", "hideBatteryIcon", "showBatteryPercent",
        "hideBleIcon", "hideVolumeIndicator", "hideRssiIndicator", "voiceAlertMode",
        "voiceDirectionEnabled", "announceBogeyCount", "muteVoiceIfVolZero", "voiceVolume",
        "announceSecondaryAlerts", "secondaryLaser", "secondaryKa", "secondaryK", "secondaryX",
        "alertVolumeFadeEnabled", "alertVolumeFadeDelaySec", "alertVolumeFadeVolume",
        "speedMuteEnabled", "speedMuteThresholdMph", "speedMuteHysteresisMph", "speedMuteVolume",
        "speedMuteVoice", "stealthEnabled", "autoPushEnabled", "autoPushProfileSchemaVersion",
        "activeSlot", "slot0Name", "slot0Color", "slot0Volume", "slot0MuteVolume",
        "slot0DarkMode", "slot0MuteToZero", "slot0AlertPersist", "slot0PriorityArrow",
        "slot0ProfileName", "slot0Mode", "slot1Name", "slot1Color", "slot1Volume",
        "slot1MuteVolume", "slot1DarkMode", "slot1MuteToZero", "slot1AlertPersist",
        "slot1PriorityArrow", "slot1ProfileName", "slot1Mode", "slot2Name", "slot2Color",
        "slot2Volume", "slot2MuteVolume", "slot2DarkMode", "slot2MuteToZero",
        "slot2AlertPersist", "slot2PriorityArrow", "slot2ProfileName", "slot2Mode", "profiles",
    };
    const JsonObjectConst root = doc.as<JsonObjectConst>();
    if (!jsonObjectHasOnlyKeys(root, kTopLevelKeys, sizeof(kTopLevelKeys) / sizeof(kTopLevelKeys[0]))) {
        return false;
    }

    // Version 21 is the first exact backup schema.  Every field emitted by the
    // current writer is required so a truncated/current-looking document can
    // never turn an omitted setting into a successful partial restore.  The
    // only shape difference is deliberate: downloadable backups omit secrets
    // and CRC, while local SD backups require both.
    const bool httpBackup = exactV1JsonToken(doc["_type"], "v1simple_backup");
    const bool sdBackup = exactV1JsonToken(doc["_type"], "v1simple_sd_backup");
    if ((!httpBackup && !sdBackup) || !doc["_version"].is<int>() ||
        doc["_version"].as<int>() != SD_BACKUP_VERSION) return false;
    for (const char* key : kTopLevelKeys) {
        const bool transportSpecific = std::strcmp(key, "apPassword") == 0 ||
                                       std::strcmp(key, "_crc32") == 0;
        if (!transportSpecific && doc[key].isUnbound()) return false;
    }
    if (sdBackup) {
        if (doc["apPassword"].isUnbound() || doc["_crc32"].isUnbound()) return false;
    } else if (!doc["apPassword"].isUnbound() || !doc["_crc32"].isUnbound()) {
        return false;
    }

    static constexpr const char* kStringKeys[] = {
        "apSSID", "apPassword", "wifiClientSSID", "proxyName", "lastV1Address",
        "obdSavedAddress", "obdSavedName", "slot0Name", "slot0ProfileName", "slot1Name",
        "slot1ProfileName", "slot2Name", "slot2ProfileName",
    };
    for (const char* key : kStringKeys) {
        if (!optionalExactString(doc, key)) return false;
    }
    if (!currentJsonStringIsValid(doc["apSSID"], MAX_WIFI_SSID_LEN, false) ||
        !currentJsonStringIsValid(doc["wifiClientSSID"], MAX_WIFI_SSID_LEN) ||
        !currentJsonStringIsValid(doc["proxyName"], MAX_PROXY_NAME_LEN, false) ||
        !currentJsonStringIsValid(doc["lastV1Address"], 17) ||
        !currentJsonStringIsTrimmed(doc["lastV1Address"]) ||
        !currentJsonStringIsValid(doc["obdSavedAddress"], 17) ||
        !currentJsonBleAddressIsValid(doc["obdSavedAddress"]) ||
        !currentJsonStringIsValid(doc["obdSavedName"], 32) ||
        !currentJsonStringIsTrimmed(doc["obdSavedName"])) return false;
    const JsonString lastAddress = doc["lastV1Address"].as<JsonString>();
    for (size_t index = 0; index < lastAddress.size(); ++index) {
        const unsigned char byte = static_cast<unsigned char>(lastAddress.c_str()[index]);
        if (byte >= 'a' && byte <= 'z') return false;
    }
    if (sdBackup && !currentJsonEncodedPasswordIsValid(doc["apPassword"])) return false;

    static constexpr const char* kBoolKeys[] = {
        "wifiClientEnabled", "proxyBLE", "obdEnabled", "alpEnabled", "alpDisableV1LaserOnPush",
        "gpsEnabled", "freqUseBandColor", "hideWifiIcon", "hideProfileIndicator", "hideBatteryIcon",
        "showBatteryPercent", "hideBleIcon", "hideVolumeIndicator", "hideRssiIndicator",
        "voiceDirectionEnabled", "announceBogeyCount", "muteVoiceIfVolZero",
        "announceSecondaryAlerts", "secondaryLaser", "secondaryKa", "secondaryK", "secondaryX",
        "alertVolumeFadeEnabled", "speedMuteEnabled", "speedMuteVoice", "stealthEnabled",
        "autoPushEnabled", "slot0DarkMode", "slot0MuteToZero", "slot0PriorityArrow",
        "slot1DarkMode", "slot1MuteToZero", "slot1PriorityArrow", "slot2DarkMode",
        "slot2MuteToZero", "slot2PriorityArrow",
    };
    for (const char* key : kBoolKeys) {
        if (!optionalExactBool(doc, key)) return false;
    }

    static constexpr const char* kColorKeys[] = {
        "colorBogey", "colorFrequency", "colorArrowFront", "colorArrowSide", "colorArrowRear",
        "colorBandL", "colorBandKa", "colorBandK", "colorBandX", "colorBandPhoto",
        "colorWiFiIcon", "colorWiFiConnected", "colorBleConnected", "colorBleDisconnected",
        "colorBar1", "colorBar2", "colorBar3", "colorBar4", "colorBar5", "colorBar6",
        "colorBarS1", "colorBarS2", "colorBarS3", "colorBarS4", "colorBarS5", "colorBarS6",
        "colorBarS7", "colorBarS8", "colorMuted", "colorPersisted", "colorVolumeMain",
        "colorVolumeMute", "colorRssiV1", "colorRssiProxy", "colorObd", "colorAlpConnected",
        "colorAlpDli", "colorAlpLidActive", "colorAlpAlert", "slot0Color", "slot1Color", "slot2Color",
    };
    for (const char* key : kColorKeys) {
        if (!optionalExactIntRange(doc, key, 1, 0xFFFF)) return false;
    }
    uint16_t configuredBarColors[6];
    for (int index = 0; index < 6; ++index) {
        char key[16];
        std::snprintf(key, sizeof(key), "colorBar%d", index + 1);
        configuredBarColors[index] = doc[key].as<uint16_t>();
    }
    uint16_t expectedCompatibilityColors[8];
    DisplayVisualContract::expandSixBarColorsToEight(configuredBarColors,
                                                      expectedCompatibilityColors);
    for (int index = 0; index < 8; ++index) {
        char key[16];
        std::snprintf(key, sizeof(key), "colorBarS%d", index + 1);
        if (doc[key].as<uint16_t>() != expectedCompatibilityColors[index]) return false;
    }

    if (!optionalExactIntRange(doc, "brightness", 1, 255) ||
        !optionalExactIntRange(doc, "autoPowerOffMinutes", 0, 60) ||
        !optionalExactIntRange(doc, "obdSavedAddrType", 0, 1) ||
        !optionalExactIntRange(doc, "obdMinRssi", -100, -40) ||
        !optionalExactIntRange(doc, "alpAlertPersistSec", 0, 5) ||
        !optionalExactIntRange(doc, "voiceAlertMode", VOICE_MODE_DISABLED, VOICE_MODE_BAND_FREQ) ||
        !optionalExactIntRange(doc, "voiceVolume", 0, 100) ||
        !optionalExactIntRange(doc, "alertVolumeFadeDelaySec", 1, 10) ||
        !optionalExactIntRange(doc, "alertVolumeFadeVolume", 1, 9) ||
        !optionalExactIntRange(doc, "speedMuteThresholdMph", 5, 60) ||
        !optionalExactIntRange(doc, "speedMuteHysteresisMph", 1, 10) ||
        !optionalExactIntRange(doc, "speedMuteVolume", 0, 9) ||
        !optionalExactIntRange(doc, "activeSlot", 0, 2)) return false;
    if (!doc["apTimeoutMinutes"].isUnbound()) {
        if (!doc["apTimeoutMinutes"].is<int>()) return false;
        const int value = doc["apTimeoutMinutes"].as<int>();
        if (value != 0 && (value < 5 || value > 60)) return false;
    }
    if (!doc["gpsBaud"].isUnbound()) {
        if (!doc["gpsBaud"].is<uint32_t>()) return false;
        const uint32_t value = doc["gpsBaud"].as<uint32_t>();
        if (value != 9600 && value != 38400 && value != 115200) return false;
    }
    if (!optionalExactUint32Range(doc, "_timestamp", 0, std::numeric_limits<uint32_t>::max()) ||
        !optionalExactUint32Range(doc, "timestamp", 0, std::numeric_limits<uint32_t>::max()) ||
        !optionalExactUint32Range(doc, "obdScanWindowMs", kConnectionCycleObdScanWindowMsMin,
                                  kConnectionCycleObdScanWindowMsMax) ||
        !optionalExactUint32Range(doc, "obdRetryIntervalMs", kConnectionCycleObdRetryIntervalMsMin,
                                  kConnectionCycleObdRetryIntervalMsMax) ||
        !optionalExactUint32Range(doc, "proxyOpenWindowMs", kConnectionCycleProxyOpenWindowMsMin,
                                  kConnectionCycleProxyOpenWindowMsMax) ||
        !optionalExactUint32Range(doc, "v1SettleQuietMs", kConnectionCycleV1SettleQuietMsMin,
                                  kConnectionCycleV1SettleQuietMsMax) ||
        !optionalExactUint32Range(doc, "v1SettleFallbackMs", kConnectionCycleV1SettleFallbackMsMin,
                                  kConnectionCycleV1SettleFallbackMsMax) ||
        !optionalExactUint32Range(doc, "cycleTeardownAckTimeoutMs", kConnectionCycleTeardownAckTimeoutMsMin,
                                  kConnectionCycleTeardownAckTimeoutMsMax)) return false;
    if (!doc["_timestamp"].isUnbound() && !doc["timestamp"].isUnbound() &&
        doc["_timestamp"].as<uint32_t>() != doc["timestamp"].as<uint32_t>()) return false;
    if (!doc["colorWiFiIcon"].isUnbound() && !doc["colorWiFiConnected"].isUnbound() &&
        doc["colorWiFiIcon"].as<int>() != doc["colorWiFiConnected"].as<int>()) return false;

    if (doc["proxyBLE"].as<bool>() && doc["obdEnabled"].as<bool>()) return false;
    if (!doc["autoPushProfileSchemaVersion"].is<int>() ||
        doc["autoPushProfileSchemaVersion"].as<int>() != V1_PROFILE_SCHEMA_VERSION) return false;
    constexpr bool profileOwnedMarker = true;

    for (int slot = 0; slot < 3; ++slot) {
        char key[24];
        std::snprintf(key, sizeof(key), "slot%dName", slot);
        const JsonString slotName = doc[key].as<JsonString>();
        if (!currentJsonStringIsValid(doc[key], MAX_SLOT_NAME_LEN) ||
            !isCanonicalSlotNameBytes(slotName.c_str(), slotName.size())) return false;
        std::snprintf(key, sizeof(key), "slot%dProfileName", slot);
        if (!currentJsonProfileNameIsCanonical(doc[key], true)) return false;
        std::snprintf(key, sizeof(key), "slot%dMode", slot);
        if (!optionalExactIntRange(doc, key, V1_MODE_UNKNOWN, V1_MODE_ADVANCED_LOGIC)) return false;
        if (profileOwnedMarker && doc[key].as<int>() != V1_MODE_UNKNOWN) return false;
        std::snprintf(key, sizeof(key), "slot%dAlertPersist", slot);
        if (!optionalExactIntRange(doc, key, 0, 5)) return false;
        char volumeKey[24], muteKey[24], darkKey[24], muteZeroKey[24];
        std::snprintf(volumeKey, sizeof(volumeKey), "slot%dVolume", slot);
        std::snprintf(muteKey, sizeof(muteKey), "slot%dMuteVolume", slot);
        std::snprintf(darkKey, sizeof(darkKey), "slot%dDarkMode", slot);
        std::snprintf(muteZeroKey, sizeof(muteZeroKey), "slot%dMuteToZero", slot);
        const auto validVolume = [&](const char* volumeName) {
            if (doc[volumeName].isUnbound()) return true;
            if (!doc[volumeName].is<int>()) return false;
            const int value = doc[volumeName].as<int>();
            return (value >= 0 && value <= 9) || value == 0xFF;
        };
        if (!validVolume(volumeKey) || !validVolume(muteKey)) return false;
        if (!doc[volumeKey].isUnbound() && !doc[muteKey].isUnbound()) {
            const int volume = doc[volumeKey].as<int>();
            const int mute = doc[muteKey].as<int>();
            if ((volume == 0xFF) != (mute == 0xFF)) return false;
        }
        if (profileOwnedMarker &&
            (doc[volumeKey].as<int>() != 0xFF || doc[muteKey].as<int>() != 0xFF ||
             doc[darkKey].as<bool>() || doc[muteZeroKey].as<bool>())) return false;
    }

    if (!doc["wifiStaSlots"].isUnbound()) {
        if (!doc["wifiStaSlots"].is<JsonArrayConst>() || doc["wifiStaSlots"].size() > kWifiStaSlotCount) {
            return false;
        }
        static constexpr const char* kWifiSlotKeys[] = {"index", "ssid", "label", "priority", "lastConnectedAtSec"};
        int primaryIndex = -1;
        int primaryPriority = 0;
        uint32_t primaryLastConnected = 0;
        const char* primarySsid = "";
        size_t primarySsidLength = 0;
        bool seen[kWifiStaSlotCount] = {};
        for (JsonVariantConst value : doc["wifiStaSlots"].as<JsonArrayConst>()) {
            if (!value.is<JsonObjectConst>()) return false;
            const JsonObjectConst slot = value.as<JsonObjectConst>();
            if (!jsonObjectHasOnlyKeys(slot, kWifiSlotKeys, sizeof(kWifiSlotKeys) / sizeof(kWifiSlotKeys[0])) ||
                slot.size() != sizeof(kWifiSlotKeys) / sizeof(kWifiSlotKeys[0]) ||
                !slot["index"].is<int>() || slot["index"].as<int>() < 0 ||
                slot["index"].as<int>() >= static_cast<int>(kWifiStaSlotCount) ||
                !slot["ssid"].is<const char*>() || !slot["label"].is<const char*>() ||
                !slot["priority"].is<int>() || slot["priority"].as<int>() < 0 ||
                slot["priority"].as<int>() > 255 || !slot["lastConnectedAtSec"].is<uint32_t>()) return false;
            const JsonString ssid = slot["ssid"].as<JsonString>();
            // The current writer represents an unused slot by omitting it.
            // Accepting an explicit empty record would let ignored metadata
            // disappear across a supposedly exact v21 round trip.
            const int index = slot["index"].as<int>();
            if (seen[index] || ssid.size() == 0 ||
                !currentJsonStringIsValid(slot["ssid"], MAX_WIFI_SSID_LEN, false) ||
                !currentJsonStringIsValid(slot["label"], MAX_WIFI_STA_LABEL_LEN, false) ||
                !currentJsonStringIsTrimmed(slot["label"])) return false;
            seen[index] = true;
            const int priority = slot["priority"].as<int>();
            const uint32_t lastConnected = slot["lastConnectedAtSec"].as<uint32_t>();
            if (primaryIndex < 0 || priority < primaryPriority ||
                (priority == primaryPriority && lastConnected > primaryLastConnected) ||
                (priority == primaryPriority && lastConnected == primaryLastConnected &&
                 index < primaryIndex)) {
                primaryIndex = index;
                primaryPriority = priority;
                primaryLastConnected = lastConnected;
                primarySsid = ssid.c_str();
                primarySsidLength = ssid.size();
            }
        }
        const JsonString alias = doc["wifiClientSSID"].as<JsonString>();
        if (alias.size() != primarySsidLength ||
            (primarySsidLength > 0 &&
             std::memcmp(alias.c_str(), primarySsid, primarySsidLength) != 0)) return false;
    }

    if (!doc["profiles"].isUnbound()) {
        if (!doc["profiles"].is<JsonArrayConst>() ||
            doc["profiles"].size() > V1_PROFILE_CATALOG_MAX_COUNT) return false;
        static constexpr const char* kVersionedProfileKeys[] = {
            "name", "description", "schemaVersion", "detector", "bytes"};
        const JsonArrayConst catalog = doc["profiles"].as<JsonArrayConst>();
        size_t profileIndex = 0;
        for (JsonVariantConst value : catalog) {
            if (!value.is<JsonObjectConst>()) return false;
            const JsonObjectConst profile = value.as<JsonObjectConst>();
            const char* const* keys = kVersionedProfileKeys;
            const size_t count = sizeof(kVersionedProfileKeys) / sizeof(kVersionedProfileKeys[0]);
            if (profile.size() != count || !jsonObjectHasOnlyKeys(profile, keys, count) ||
                !currentJsonProfileNameIsCanonical(profile["name"]) ||
                !currentJsonStringIsValid(profile["description"], V1_PROFILE_DESCRIPTION_MAX_BYTES) ||
                !currentJsonRawSettingsAreValid(profile["bytes"])) return false;
            if (!profile["schemaVersion"].is<int>() ||
                profile["schemaVersion"].as<int>() != V1_PROFILE_SCHEMA_VERSION ||
                !profile["detector"].is<JsonObjectConst>() ||
                !currentJsonDetectorIsValid(profile["detector"].as<JsonObjectConst>())) return false;
            size_t priorIndex = 0;
            for (JsonVariantConst priorValue : catalog) {
                if (priorIndex++ >= profileIndex) break;
                if (currentJsonProfileNamesCollide(priorValue["name"], profile["name"])) return false;
            }
            ++profileIndex;
        }
        for (int slot = 0; slot < 3; ++slot) {
            char assignmentKey[24];
            std::snprintf(assignmentKey, sizeof(assignmentKey), "slot%dProfileName", slot);
            const JsonString assignment = doc[assignmentKey].as<JsonString>();
            if (assignment.size() == 0) continue;
            bool found = false;
            for (JsonVariantConst value : catalog) {
                const JsonString name = value["name"].as<JsonString>();
                found = found || (assignment.size() == name.size() &&
                                  std::memcmp(assignment.c_str(), name.c_str(), name.size()) == 0);
            }
            if (!found) return false;
        }
    }
    return true;
}

bool parseBackupProfile(JsonObjectConst source, V1Profile& profile) {
    if (!source["name"].is<const char*>() ||
        !V1SettingsJson::parseRawBytes(source["bytes"], profile.settings.bytes)) {
        return false;
    }
    String rawName;
    String canonical;
    if (!exactV1JsonString(source["name"], rawName, MAX_PROFILE_NAME_LEN) ||
        canonicalizeProfileName(rawName, canonical) != ProfileNameStatus::Valid || canonical != rawName) {
        return false;
    }
    profile.name = canonical;
    const bool hasSchemaVersion = !source["schemaVersion"].isUnbound();
    const bool hasDetector = !source["detector"].isUnbound();
    if (hasSchemaVersion != hasDetector) return false;
    if (hasSchemaVersion) {
        if (!source["schemaVersion"].is<int>() ||
            (source["schemaVersion"].as<int>() != V1_PROFILE_PREVIOUS_SCHEMA_VERSION &&
             source["schemaVersion"].as<int>() != V1_PROFILE_SCHEMA_VERSION) ||
            !source["detector"].is<JsonObjectConst>() ||
            !(source["schemaVersion"].as<int>() == V1_PROFILE_SCHEMA_VERSION
                  ? parseV1DetectorConfiguration(source["detector"].as<JsonObjectConst>(), profile.detector)
                  : parseV1DetectorConfigurationV2(source["detector"].as<JsonObjectConst>(), profile.detector))) {
            return false;
        }
        if (source["schemaVersion"].as<int>() == V1_PROFILE_PREVIOUS_SCHEMA_VERSION) {
            profile.detector = migrateV1DetectorConfigurationV2(profile.detector);
        }
        profile.schemaVersion = V1_PROFILE_SCHEMA_VERSION;
    } else {
        profile.schemaVersion = 1;
    }
    if (!source["description"].isNull()) {
        // Descriptions already accepted by profile storage must round trip,
        // including through the rollback journal that uses this parser.
        if (!exactV1JsonString(source["description"], profile.description,
                               V1_PROFILE_DESCRIPTION_MAX_BYTES)) return false;
    } else if (hasSchemaVersion) return false;
    if (!hasSchemaVersion && !source["displayOn"].isNull()) {
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
    if (hasSchemaVersion) {
        return source["displayOn"].isUnbound() && source["mainVolume"].isUnbound() &&
               source["mutedVolume"].isUnbound();
    }
    return readVolume("mainVolume", profile.mainVolume) && readVolume("mutedVolume", profile.mutedVolume);
}

bool validateBackupNetworkCredentialFields(const JsonDocument& doc, PreparedNetworkFields* output = nullptr) {
    PreparedNetworkFields prepared;
    if (!prepareNetworkFields(doc, BackupRestoreScope::Full, prepared)) return false;
    if (output) *output = std::move(prepared);
    return true;
}

bool validateBackupDocumentForApply(const JsonDocument& doc, const V1Settings& current, V1ProfileManager& profiles,
                                    std::vector<V1Profile>& incomingProfiles,
                                    std::vector<V1Profile>& existingProfiles, bool replaceProfiles = false,
                                    PreparedProfileSlotFields* preparedSlots = nullptr,
                                    PreparedNetworkFields* preparedNetwork = nullptr,
                                    PreparedObdFields* preparedObd = nullptr) {
    if (!doc.is<JsonObjectConst>()) {
        return false;
    }
    if (!doc["_type"].isUnbound() &&
        (!doc["_type"].is<const char*>() ||
         !BackupPayloadBuilder::isRecognizedBackupType(doc["_type"]))) {
        return false;
    }
    if (!doc["_crc32"].isUnbound()) {
        if (!doc["_crc32"].is<uint32_t>() ||
            doc["_crc32"].as<uint32_t>() != BackupPayloadBuilder::computeBackupCrc32(doc)) {
            return false;
        }
    }
    const JsonVariantConst currentVersion = doc["_version"];
    const JsonVariantConst legacyVersion = doc["version"];
    if ((!currentVersion.isUnbound() &&
         (!currentVersion.is<int>() || currentVersion.as<int>() < 1 ||
          currentVersion.as<int>() > SD_BACKUP_VERSION)) ||
        (!legacyVersion.isUnbound() &&
         (!legacyVersion.is<int>() || legacyVersion.as<int>() < 1 ||
          legacyVersion.as<int>() > SD_BACKUP_VERSION)) ||
        (!currentVersion.isUnbound() && !legacyVersion.isUnbound() &&
         currentVersion.as<int>() != legacyVersion.as<int>())) return false;
    const bool claimsCurrentVersion =
        (currentVersion.is<int>() && currentVersion.as<int>() == SD_BACKUP_VERSION) ||
        (legacyVersion.is<int>() && legacyVersion.as<int>() == SD_BACKUP_VERSION);
    // Version 21 is the first exact-schema backup. Its writer marker is
    // `_version`; accepting the historical alias here would let a v21 claim
    // retain legacy open-ended semantics.
    if (claimsCurrentVersion &&
        (currentVersion.isUnbound() || !validateCurrentBackupSchema(doc))) return false;
    if ((replaceProfiles && !doc["profiles"].is<JsonArrayConst>()) ||
        (!doc["profiles"].isNull() && !doc["profiles"].is<JsonArrayConst>())) {
        return false;
    }
    if (doc["profiles"].is<JsonArrayConst>() &&
        doc["profiles"].size() > V1_PROFILE_CATALOG_MAX_COUNT) return false;
    if (!validateBackupNetworkCredentialFields(doc, preparedNetwork)) {
        return false;
    }
    PreparedObdFields localObd;
    if (!prepareObdFields(doc, localObd)) return false;
    if (preparedObd) *preparedObd = std::move(localObd);
    const JsonVariantConst profileSchemaMarker = doc["autoPushProfileSchemaVersion"];
    if (!profileSchemaMarker.isUnbound() &&
        (!profileSchemaMarker.is<int>() ||
         (profileSchemaMarker.as<int>() != 0 &&
          profileSchemaMarker.as<int>() != V1_PROFILE_PREVIOUS_SCHEMA_VERSION &&
          profileSchemaMarker.as<int>() != V1_PROFILE_SCHEMA_VERSION))) {
        return false;
    }

    if (profiles.isReady()) {
        const ProfileOperationResult snapshot = profiles.snapshotProfiles(existingProfiles, 250);
        if (!snapshot.success()) {
            return false;
        }
    }

    if (doc["profiles"].is<JsonArrayConst>()) {
        if (!profiles.isReady()) {
            return false;
        }
        try {
            incomingProfiles.reserve(doc["profiles"].size());
            for (JsonVariantConst value : doc["profiles"].as<JsonArrayConst>()) {
                if (!value.is<JsonObjectConst>()) {
                    return false;
                }
                V1Profile profile;
                if (!parseBackupProfile(value.as<JsonObjectConst>(), profile)) {
                    return false;
                }
                for (const V1Profile& prior : incomingProfiles) {
                    if (profileCanonicalNamesCollide(prior.name, profile.name)) {
                        return false;
                    }
                }
                incomingProfiles.push_back(std::move(profile));
            }
        } catch (const std::bad_alloc&) {
            incomingProfiles.clear();
            existingProfiles.clear();
            return false;
        }
    }

    size_t availableCount = incomingProfiles.size();
    if (!replaceProfiles) {
        for (const V1Profile& existing : existingProfiles) {
            bool replacedByIncoming = false;
            for (const V1Profile& incoming : incomingProfiles) {
                replacedByIncoming |= incoming.name == existing.name;
            }
            if (!replacedByIncoming) ++availableCount;
        }
    }
    if (availableCount > V1_PROFILE_CATALOG_MAX_COUNT) return false;

    const bool markerIsVersioned = profileSchemaMarker.is<int>() &&
        (profileSchemaMarker.as<int>() == V1_PROFILE_PREVIOUS_SCHEMA_VERSION ||
         profileSchemaMarker.as<int>() == V1_PROFILE_SCHEMA_VERSION);
    if (markerIsVersioned) {
        if (!doc["profiles"].is<JsonArrayConst>()) return false;
        for (const V1Profile& profile : incomingProfiles) {
            if (profile.schemaVersion != V1_PROFILE_SCHEMA_VERSION) return false;
        }
        for (int slot = 0; slot < 3; ++slot) {
            char modeKey[16], volumeKey[24], muteKey[24], darkKey[24], muteZeroKey[24];
            std::snprintf(modeKey, sizeof(modeKey), "slot%dMode", slot);
            std::snprintf(volumeKey, sizeof(volumeKey), "slot%dVolume", slot);
            std::snprintf(muteKey, sizeof(muteKey), "slot%dMuteVolume", slot);
            std::snprintf(darkKey, sizeof(darkKey), "slot%dDarkMode", slot);
            std::snprintf(muteZeroKey, sizeof(muteZeroKey), "slot%dMuteToZero", slot);
            if ((!doc[modeKey].isUnbound() && (!doc[modeKey].is<int>() || doc[modeKey].as<int>() != 0)) ||
                (!doc[volumeKey].isUnbound() && (!doc[volumeKey].is<int>() || doc[volumeKey].as<int>() != 255)) ||
                (!doc[muteKey].isUnbound() && (!doc[muteKey].is<int>() || doc[muteKey].as<int>() != 255)) ||
                (!doc[darkKey].isUnbound() && (!doc[darkKey].is<bool>() || doc[darkKey].as<bool>())) ||
                (!doc[muteZeroKey].isUnbound() &&
                 (!doc[muteZeroKey].is<bool>() || doc[muteZeroKey].as<bool>()))) {
                return false;
            }
        }
    } else {
        for (const V1Profile& profile : incomingProfiles) {
            if (profile.schemaVersion != 1) return false;
        }
    }

    const String* currentAssignmentSources[3] = {&current.slot0_default.profileName,
                                                 &current.slot1_highway.profileName,
                                                 &current.slot2_comfort.profileName};
    for (int slot = 0; slot < 3; ++slot) {
        char key[24];
        std::snprintf(key, sizeof(key), "slot%dProfileName", slot);
        String assigned;
        if (!exactStringCopy(*currentAssignmentSources[slot], assigned)) return false;
        if (!doc[key].isUnbound()) {
            const ExactV1JsonStringStatus stringStatus =
                exactV1JsonStringChecked(doc[key], assigned, MAX_PROFILE_NAME_LEN);
            if (stringStatus != ExactV1JsonStringStatus::Valid) return false;
            String canonicalAssigned;
            if (assigned.length() > 0 &&
                (canonicalizeProfileName(assigned, canonicalAssigned) != ProfileNameStatus::Valid ||
                 canonicalAssigned != assigned)) return false;
        }
        const bool hasDocumentAssignment = !doc[key].isUnbound();
        if (assigned.length() == 0) {
            if (hasDocumentAssignment && preparedSlots) {
                preparedSlots->hasProfileName[slot] = true;
                preparedSlots->profileNames[slot] = String();
            }
            std::snprintf(key, sizeof(key), "slot%dName", slot);
            if (!doc[key].isUnbound()) {
                String displayName;
                if (exactV1JsonStringChecked(doc[key], displayName, MAX_SLOT_NAME_LEN) !=
                    ExactV1JsonStringStatus::Valid) return false;
                if (preparedSlots) {
                    preparedSlots->hasDisplayName[slot] = true;
                    preparedSlots->displayNames[slot] = std::move(displayName);
                }
            }
            continue;
        }
        String canonical;
        if (canonicalizeProfileName(assigned, canonical) != ProfileNameStatus::Valid || canonical != assigned) {
            return false;
        }
        // References must preserve the catalog spelling used by profile loads
        // and USB export; case folding is only for detecting name collisions.
        bool found = false;
        for (const V1Profile& incoming : incomingProfiles) {
            found |= incoming.name == canonical;
        }
        if (!replaceProfiles) {
            for (const V1Profile& existing : existingProfiles) {
                found |= existing.name == canonical;
            }
        }
        if (!found) {
            return false;
        }
        if (markerIsVersioned) {
            bool foundInV2Document = false;
            for (const V1Profile& candidate : incomingProfiles) {
                foundInV2Document |= candidate.name == canonical &&
                                     candidate.schemaVersion == V1_PROFILE_SCHEMA_VERSION;
            }
            if (!foundInV2Document) return false;
        }
        if (hasDocumentAssignment && preparedSlots) {
            preparedSlots->hasProfileName[slot] = true;
            preparedSlots->profileNames[slot] = std::move(assigned);
        }

        std::snprintf(key, sizeof(key), "slot%dName", slot);
        if (!doc[key].isUnbound()) {
            String displayName;
            if (exactV1JsonStringChecked(doc[key], displayName, MAX_SLOT_NAME_LEN) !=
                ExactV1JsonStringStatus::Valid) return false;
            if (preparedSlots) {
                preparedSlots->hasDisplayName[slot] = true;
                preparedSlots->displayNames[slot] = std::move(displayName);
            }
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
        bool existedBefore = false;
        for (const V1Profile& prior : before) {
            existedBefore |= prior.name == profile.name;
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

constexpr const char* RESTORE_TRANSACTION_PATH = "/v1restore_transaction.json";
constexpr const char* RESTORE_TRANSACTION_TMP_PATH = "/v1restore_transaction.tmp";
constexpr const char* RESTORE_TRANSACTION_TYPE = "v1simple_restore_transaction";
constexpr int RESTORE_TRANSACTION_VERSION = 2;
constexpr size_t RESTORE_TRANSACTION_MAX_BYTES = 128 * 1024;

constexpr const char* PROFILE_DELETE_TRANSACTION_PATH = "/v1profile_delete_transaction.json";
constexpr const char* PROFILE_DELETE_TRANSACTION_TMP_PATH = "/v1profile_delete_transaction.tmp";
constexpr const char* PROFILE_DELETE_TRANSACTION_TYPE = "v1simple_profile_delete_transaction";
constexpr int PROFILE_DELETE_TRANSACTION_VERSION = 2;
// The measured maximal schema-v3 delete journal is pinned by native tests.
// Round its compact representation to the next 4 KiB quantum so deleting a
// legal maximal profile remains available, including as the over-cap prune
// path for grandfathered catalogs.
constexpr size_t PROFILE_DELETE_TRANSACTION_MAX_BYTES = 16 * 1024;

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
    MemoryUnavailable,
    Parsed,
    Valid,
};

struct JournalCopy {
    JournalCopyStatus status = JournalCopyStatus::Missing;
    PsramJson::Document doc;
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
    return lhs.name == rhs.name && lhs.description == rhs.description && lhs.schemaVersion == rhs.schemaVersion &&
           lhs.detector == rhs.detector && lhs.displayOn == rhs.displayOn &&
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
    target["schemaVersion"] = V1_PROFILE_SCHEMA_VERSION;
    target["name"] = profile.name;
    target["description"] = profile.description;
    appendV1DetectorConfiguration(target["detector"].to<JsonObject>(), profile.detector);
    JsonArray bytes = target["bytes"].to<JsonArray>();
    for (uint8_t byte : profile.settings.bytes) {
        bytes.add(byte);
    }
}

bool readProfileFromJournal(JsonObjectConst source, V1Profile& profile) {
    const bool legacy = source["schemaVersion"].isUnbound();
    const size_t expectedSize = legacy ? 6 : 5;
    return jsonHasExactSize(source, expectedSize) && source["name"].is<const char*>() &&
           source["description"].is<const char*>() && source["bytes"].is<JsonArrayConst>() &&
           (legacy ? (source["displayOn"].is<bool>() && source["mainVolume"].is<uint8_t>() &&
                      source["mutedVolume"].is<uint8_t>())
                   : source["detector"].is<JsonObjectConst>()) &&
           parseBackupProfile(source, profile);
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
            String parsed;
            if (!exactV1JsonString(slot["value"], parsed, RESTORE_TRANSACTION_MAX_BYTES)) {
                return false;
            }
            snapshot.passwordValues[index] = std::move(parsed);
        }
        ++index;
    }
    snapshot.legacyPasswordPresent = source["legacyPresent"].as<bool>();
    if (snapshot.legacyPasswordPresent) {
        String parsed;
        if (!exactV1JsonString(source["legacyValue"], parsed, RESTORE_TRANSACTION_MAX_BYTES)) {
            return false;
        }
        snapshot.legacyPasswordValue = std::move(parsed);
    }
    snapshot.sdRelevant = source["sdRelevant"].as<bool>();
    snapshot.sdFilePresent = source["sdPresent"].as<bool>();
    if (!snapshot.sdRelevant && snapshot.sdFilePresent) {
        return false;
    }
    if (snapshot.sdRelevant && snapshot.sdFilePresent) {
        String parsed;
        if (!exactV1JsonString(source["sdBytes"], parsed, RESTORE_TRANSACTION_MAX_BYTES)) {
            return false;
        }
        snapshot.sdFileBytes = std::move(parsed);
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
    const size_t expected = measureJson(doc);
    if (!storage.isReady() || !path || !tempPath || doc.overflowed() || expected == 0 || expected > maxBytes) {
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
        const size_t written = serializeJson(doc, file);
        file.flush();
        file.close();
        if (written != expected) {
            fs->remove(tempPath);
            return false;
        }
        File verify = fs->open(tempPath, FILE_READ);
        const size_t verifySize = verify ? verify.size() : 0;
        PsramJson::Buffer bytes(verifySize);
        PsramJson::Document verified;
        const bool valid = verify && verifySize == written && bytes &&
                           verify.read(bytes.data(), verifySize) == verifySize &&
                           ExactJsonInput::validate(bytes.data(), verifySize) == ExactJsonInput::Status::Ok &&
                           !deserializeJson(verified, bytes.data(), verifySize) && !verified.overflowed() &&
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
        const size_t fileSize = file.size();
        PsramJson::Buffer bytes(fileSize);
        if (!bytes) {
            file.close();
            copy.status = JournalCopyStatus::MemoryUnavailable;
            return;
        }
        const size_t bytesRead = file.read(bytes.data(), fileSize);
        file.close();
        if (bytesRead != fileSize) {
            copy.status = JournalCopyStatus::Invalid;
            return;
        }
        const ExactJsonInput::Status exact = ExactJsonInput::validate(bytes.data(), fileSize);
        if (exact != ExactJsonInput::Status::Ok) {
            copy.status = exact == ExactJsonInput::Status::MemoryUnavailable
                              ? JournalCopyStatus::MemoryUnavailable
                              : JournalCopyStatus::Invalid;
            return;
        }
        const DeserializationError error = deserializeJson(copy.doc, bytes.data(), fileSize);
        if (error == DeserializationError::NoMemory || copy.doc.overflowed()) {
            copy.status = JournalCopyStatus::MemoryUnavailable;
            return;
        }
        if (error) {
            copy.status = JournalCopyStatus::Invalid;
            return;
        }
        copy.status = JournalCopyStatus::Parsed;
    };
    readOne(mirrors.primaryFs, mirrors.primary);
    readOne(mirrors.secondaryFs, mirrors.secondary);
    return mirrors.primary.status != JournalCopyStatus::MemoryUnavailable &&
           mirrors.secondary.status != JournalCopyStatus::MemoryUnavailable;
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

bool writeRestoreTransactionJournal(StorageManager& storage, uint64_t token, bool credentialsMutated,
                                    const RestoreCredentialSnapshot& credentialsBefore, bool profilesMutated,
                                    const std::vector<V1Profile>& profilesBefore) {
    PsramJson::Document doc;
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

JournalCopyStatus readRestoreTransactionJournal(const JsonDocument& doc,
                                                RestoreTransactionJournal& journal) {
    if (!exactV1JsonToken(doc["_type"], RESTORE_TRANSACTION_TYPE) ||
        !doc["_version"].is<int>() || doc["_version"].as<int>() != RESTORE_TRANSACTION_VERSION ||
        !doc["token"].is<int64_t>() || doc["token"].as<int64_t>() <= 0 ||
        !doc["credentialsMutated"].is<bool>() || !doc["profilesMutated"].is<bool>() ||
        !journalCrcValid(doc)) {
        return JournalCopyStatus::Invalid;
    }
    journal.token = static_cast<uint64_t>(doc["token"].as<int64_t>());
    journal.credentialsMutated = doc["credentialsMutated"].as<bool>();
    if (journal.credentialsMutated &&
        (!doc["credentialsBefore"].is<JsonObjectConst>() ||
         !readCredentialSnapshotFromJournal(doc["credentialsBefore"].as<JsonObjectConst>(),
                                            journal.credentialsBefore))) {
        return JournalCopyStatus::Invalid;
    }
    journal.profilesMutated = doc["profilesMutated"].as<bool>();
    if (journal.profilesMutated) {
        if (!doc["profilesBefore"].is<JsonArrayConst>() ||
            doc["profilesBefore"].size() > V1_PROFILE_CATALOG_MAX_COUNT) {
            return JournalCopyStatus::Invalid;
        }
        try {
            journal.profilesBefore.reserve(doc["profilesBefore"].size());
            for (JsonVariantConst value : doc["profilesBefore"].as<JsonArrayConst>()) {
                if (!value.is<JsonObjectConst>()) {
                    return JournalCopyStatus::Invalid;
                }
                V1Profile profile;
                if (!readProfileFromJournal(value.as<JsonObjectConst>(), profile)) {
                    return JournalCopyStatus::Invalid;
                }
                journal.profilesBefore.push_back(std::move(profile));
            }
        } catch (const std::bad_alloc&) {
            journal.profilesBefore.clear();
            return JournalCopyStatus::MemoryUnavailable;
        }
    }
    const size_t expectedKeys = 6 + (journal.credentialsMutated ? 1 : 0) +
                                (journal.profilesMutated ? 1 : 0);
    return jsonHasExactSize(doc.as<JsonObjectConst>(), expectedKeys)
               ? JournalCopyStatus::Valid
               : JournalCopyStatus::Invalid;
}

bool writeProfileDeleteTransactionJournal(StorageManager& storage, uint64_t token, const V1Profile& profile,
                                          bool hadReferences) {
    PsramJson::Document doc;
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

JournalCopyStatus readProfileDeleteTransactionJournal(const JsonDocument& doc,
                                                      ProfileDeleteTransactionJournal& journal) {
    if (!exactV1JsonToken(doc["_type"], PROFILE_DELETE_TRANSACTION_TYPE) ||
        !doc["_version"].is<int>() || doc["_version"].as<int>() != PROFILE_DELETE_TRANSACTION_VERSION ||
        !doc["token"].is<int64_t>() || doc["token"].as<int64_t>() <= 0 ||
        !doc["hadReferences"].is<bool>() || !doc["profile"].is<JsonObjectConst>() ||
        !journalCrcValid(doc) || !jsonHasExactSize(doc.as<JsonObjectConst>(), 6)) {
        return JournalCopyStatus::Invalid;
    }
    journal.token = static_cast<uint64_t>(doc["token"].as<int64_t>());
    journal.hadReferences = doc["hadReferences"].as<bool>();
    return readProfileFromJournal(doc["profile"].as<JsonObjectConst>(), journal.profile)
               ? JournalCopyStatus::Valid
               : JournalCopyStatus::Invalid;
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
            copy.status = readJournal(copy.doc, journal);
        }
    };
    validateCopy(mirrors.primary, primaryJournal);
    validateCopy(mirrors.secondary, secondaryJournal);

    // A parsed journal whose bounded C++ staging could not be allocated is
    // still authoritative unknown state, not corrupt input. Never discard it
    // in favor of another mirror or remove it as an invalid copy.
    if (mirrors.primary.status == JournalCopyStatus::MemoryUnavailable ||
        mirrors.secondary.status == JournalCopyStatus::MemoryUnavailable) {
        return false;
    }

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
            String encoded;
            String decoded;
            if (!readBackupPreferenceStringExact(prefs, kNvsWifiStaSlotPassword[i],
                                                 kMaxEncodedWifiPasswordBytes, encoded) ||
                !decodeObfuscatedChecked(encoded, decoded)) {
                prefs.end();
                return false;
            }
            snapshot.passwordValues[i] = std::move(encoded);
        }
    }
    snapshot.legacyPasswordPresent = prefs.isKey(kNvsWifiPassword);
    if (snapshot.legacyPasswordPresent) {
        String encoded;
        String decoded;
        if (!readBackupPreferenceStringExact(prefs, kNvsWifiPassword,
                                             kMaxEncodedWifiPasswordBytes, encoded) ||
            !decodeObfuscatedChecked(encoded, decoded)) {
            prefs.end();
            return false;
        }
        snapshot.legacyPasswordValue = std::move(encoded);
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
    if (expected == 0 || expected > kMaxWifiSecretSnapshotBytes) {
        file.close();
        return false;
    }
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
        bool verified = written == value.length() && prefs.isKey(key) &&
                        prefs.getStringLength(key) == value.length() + 1u;
        if (verified && value.length() > 0) {
            String readBack;
            verified = readBackupPreferenceStringExact(prefs, key, value.length(), readBack) &&
                       readBack == value;
        }
        restored = verified && restored;
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

bool validateCurrentBackupDocumentShape(const JsonDocument& doc) {
    return validateCurrentBackupSchema(doc);
}

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
            if (!profiles_->restoreProfileForTransaction(journal.profile).success) {
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

    V1Settings settingsBeforeDelete;
    if (!copySettingsChecked(settings_, settingsBeforeDelete)) {
        resolveProfileDeleteTransaction();
        return profileOperationResult(ProfileStorageStatus::IoError,
                                      "Settings snapshot memory unavailable during profile delete");
    }
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
        settings_ = std::move(settingsBeforeDelete);
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
                                                               const SettingsRestoreWatchdog& watchdog,
                                                               SettingsBackupScope scope) {
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

    std::unique_ptr<RestoreApplyStaging> staging;
#ifdef UNIT_TEST
    if (restoreApplyStagingAllocationFailure_) {
        restoreApplyStagingAllocationFailure_ = false;
        Serial.println("[Settings] ERROR: Restore transaction staging memory unavailable");
        return result;
    }
#endif
    staging.reset(new (std::nothrow) RestoreApplyStaging());
    if (!staging) {
        Serial.println("[Settings] ERROR: Restore transaction staging memory unavailable");
        return result;
    }

    V1Settings& settingsBefore = staging->settingsBefore;
    std::vector<V1Profile>& incomingProfiles = staging->incomingProfiles;
    std::vector<V1Profile>& profilesBefore = staging->profilesBefore;
    PreparedProfileSlotFields& preparedSlots = staging->preparedSlots;
    PreparedNetworkFields& preparedNetwork = staging->preparedNetwork;
    PreparedObdFields& preparedObd = staging->preparedObd;
    RestoreCredentialSnapshot& credentialsBefore = staging->credentialsBefore;
    if (!copySettingsChecked(settings_, settingsBefore)) {
        Serial.println("[Settings] ERROR: Settings snapshot memory unavailable before restore");
        return result;
    }
    const bool restorePendingBefore = restorePending_;
    const uint64_t restoreWatermarkBefore = restoreCommitWatermark_;
    const bool profilesOnly = scope == SettingsBackupScope::ProfilesOnly;
    if (!validateBackupDocumentForApply(doc, settingsBefore, *profiles_, incomingProfiles, profilesBefore,
                                        profilesOnly, &preparedSlots, &preparedNetwork, &preparedObd)) {
        Serial.println("[Settings] ERROR: Backup document failed transaction validation");
        return result;
    }
    if (!profilesOnly && !captureRestoreCredentialSnapshot(*storage_, credentialsBefore)) {
        Serial.println("[Settings] ERROR: Failed to snapshot credential stores before restore");
        return result;
    }

    const bool credentialsMutated = !profilesOnly && preparedNetwork.mutatesCredentials;
    const bool profilesMutated = !incomingProfiles.empty() || (profilesOnly && !profilesBefore.empty());
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
        settings_ = std::move(settingsBefore);
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

    if (!profilesOnly &&
        !applyPreparedNetworkFields(doc, preparedNetwork, settings_, *storage_, BackupRestoreScope::Full,
                                    deferBackupRewrite)) {
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

    if (!profilesOnly) {
        applyBackupDisplayFields(doc, settings_, BackupRestoreScope::Full);
        applyBackupAudioFields(doc, settings_, BackupRestoreScope::Full);
    }
    applyBackupProfileSlotFields(doc, settings_, BackupRestoreScope::Full, preparedSlots);
    if (!profilesOnly) {
        applyPreparedObdFields(doc, settings_, BackupRestoreScope::Full, preparedObd);
        applyBackupAlpAndGpsFields(doc, settings_);
        healBackupRestoreConflicts(settings_, "restored");
    } else {
        const auto beforeSlot = settingsBefore.autoPushSlotView(settingsBefore.activeSlot);
        const auto afterSlot = settings_.autoPushSlotView(settings_.activeSlot);
        // As with ordinary settings setters, count an attempted A -> B -> A
        // even if persistence later rolls back; this is not a durable revision.
        if ((settingsBefore.activeSlot != settings_.activeSlot || beforeSlot.alertPersist != afterSlot.alertPersist ||
             beforeSlot.priorityArrow != afterSlot.priorityArrow) && displayConfigurationRevision_ != UINT32_MAX) {
            ++displayConfigurationRevision_;
        }
    }
    feedWatchdog();

    if (profilesOnly) {
        // Exact catalog replacement shares the same rollback snapshot and NVS
        // commit watermark as the profile writes below. Retire absent names
        // first so a case-only name replacement can be saved and recovered.
        for (const V1Profile& prior : profilesBefore) {
            bool retained = false;
            for (const V1Profile& incoming : incomingProfiles) {
                retained |= prior.name == incoming.name;
            }
            if (!retained && !profiles_->deleteProfileResult(prior.name, 250).success()) {
                Serial.println("[Settings] ERROR: Profile replacement failed; rolling back document");
                rollback();
                return result;
            }
            feedWatchdog();
        }
    }

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

bool SettingsManager::migrateAutoPushProfilesToV2() {
    if (!profiles_ || !profiles_->isReady() || !resolveStorageTransactionsForMutation()) return false;

    std::vector<V1Profile> catalog;
    // Pre-cap installations may contain more profiles than the supported
    // complete-catalog envelope. Migration may inspect that catalog, but it
    // never writes a lossy/truncated replacement; the operator can delete
    // profiles until the bounded migration is representable.
    if (!profiles_->snapshotProfiles(catalog, 250, true).success()) return false;
    if (catalog.size() > V1_PROFILE_CATALOG_MAX_COUNT) return false;
    if (settings_.autoPushProfileSchemaVersion == V1_PROFILE_SCHEMA_VERSION) {
        for (const V1Profile& profile : catalog) {
            if (profile.schemaVersion != V1_PROFILE_SCHEMA_VERSION) return false;
        }
        const V1Settings& state = settings_;
        for (int slotIndex = 0; slotIndex < 3; ++slotIndex) {
            const auto slot = state.autoPushSlotView(slotIndex);
            if (slot.config.mode != V1_MODE_UNKNOWN ||
                slot.volume != 0xFF || slot.muteVolume != 0xFF || slot.darkMode || slot.muteToZero) {
                return false;
            }
            if (slot.config.profileName.length() == 0) continue;
            bool found = false;
            for (const V1Profile& profile : catalog) found |= profile.name == slot.config.profileName;
            if (!found) return false;
        }
        return true;
    }
    if (settings_.autoPushProfileSchemaVersion == V1_PROFILE_PREVIOUS_SCHEMA_VERSION) {
        // Profile-file reads already validated the v2 detector CRC and
        // migrated each object in memory. Re-emit the complete catalog as v3
        // through the existing atomic ProfilesOnly restore transaction.
        for (const V1Profile& profile : catalog) {
            if (profile.schemaVersion != V1_PROFILE_SCHEMA_VERSION) return false;
        }
        PsramJson::Document migration;
        migration["autoPushEnabled"] = settings_.autoPushEnabled;
        migration["activeSlot"] = settings_.activeSlot;
        migration["autoPushProfileSchemaVersion"] = V1_PROFILE_SCHEMA_VERSION;
        for (int slotIndex = 0; slotIndex < 3; ++slotIndex) {
            const auto slot = settings_.autoPushSlotView(slotIndex);
            char key[32];
            std::snprintf(key, sizeof(key), "slot%dProfileName", slotIndex);
            migration[key] = slot.config.profileName;
            std::snprintf(key, sizeof(key), "slot%dMode", slotIndex);
            migration[key] = 0;
            std::snprintf(key, sizeof(key), "slot%dVolume", slotIndex);
            migration[key] = 255;
            std::snprintf(key, sizeof(key), "slot%dMuteVolume", slotIndex);
            migration[key] = 255;
            std::snprintf(key, sizeof(key), "slot%dDarkMode", slotIndex);
            migration[key] = false;
            std::snprintf(key, sizeof(key), "slot%dMuteToZero", slotIndex);
            migration[key] = false;
        }
        JsonArray migratedProfiles = migration["profiles"].to<JsonArray>();
        for (const V1Profile& profile : catalog) {
            JsonObject entry = migratedProfiles.add<JsonObject>();
            entry["schemaVersion"] = V1_PROFILE_SCHEMA_VERSION;
            entry["name"] = profile.name;
            entry["description"] = profile.description;
            appendV1DetectorConfiguration(entry["detector"].to<JsonObject>(), profile.detector);
            JsonArray bytes = entry["bytes"].to<JsonArray>();
            for (uint8_t byte : profile.settings.bytes) bytes.add(byte);
        }
        if (migration.overflowed()) return false;
#ifdef UNIT_TEST
        if (autoPushMigrationInterruptAfterProfiles_) {
            autoPushMigrationInterruptAfterProfiles_ = false;
            restoreInterruptAfterProfiles_ = true;
        }
#endif
        V1Settings preMigrationSettings;
        if (!copySettingsChecked(settings_, preMigrationSettings)) return false;
        const SettingsBackupApplyResult migrated =
            applyBackupDocument(migration, true, SettingsRestoreWatchdog{},
                                SettingsBackupScope::ProfilesOnly);
        if (!migrated.success) settings_ = std::move(preMigrationSettings);
        return migrated.success;
    }
    struct Variant {
        String sourceName;
        String effectiveName;
        V1Profile profile;
    };
    std::vector<V1Profile> legacyCatalog;
    std::vector<Variant> variants;
    try {
        // Secure every STL growth needed by legacy migration before changing
        // the local candidate or entering applyBackupDocument(), whose journal
        // makes later failures transactional rather than safely retryable.
        catalog.reserve(V1_PROFILE_CATALOG_MAX_COUNT);
        legacyCatalog.reserve(catalog.size());
        variants.reserve(3);
    } catch (const std::bad_alloc&) {
        return false;
    }
    for (const V1Profile& source : catalog) {
        V1Profile copy = source;
        if (!profileStringFieldsEqual(source, copy)) return false;
        try {
            legacyCatalog.push_back(std::move(copy));
        } catch (const std::bad_alloc&) {
            return false;
        }
        if (!profileStringFieldsEqual(source, legacyCatalog.back())) return false;
    }

    // Upgrade unassigned catalog entries without activating the old, unused
    // profile metadata fields. User bytes were the only profile-owned command
    // in the legacy executor.
    for (V1Profile& profile : catalog) {
        profile.schemaVersion = V1_PROFILE_SCHEMA_VERSION;
        profile.detector = V1DetectorConfiguration{};
    }

    const auto findLegacy = [&](const String& name) -> const V1Profile* {
        for (const V1Profile& profile : legacyCatalog) {
            if (profile.name == name) return &profile;
        }
        return nullptr;
    };
    const auto catalogIndex = [&](const String& name) -> int {
        for (size_t i = 0; i < catalog.size(); ++i) {
            if (catalog[i].name == name) return static_cast<int>(i);
        }
        return -1;
    };
    const auto nameAvailable = [&](const String& name) {
        for (const V1Profile& profile : catalog) {
            if (profileCanonicalNamesCollide(profile.name, name)) return false;
        }
        return true;
    };
    const auto uniqueName = [&](const String& stem, const char* fixedSuffix) {
        for (unsigned collisionOrdinal = 1; collisionOrdinal < 1000; ++collisionOrdinal) {
            String candidate;
            if (!buildMigratedProfileNameCandidate(stem, fixedSuffix, collisionOrdinal, candidate)) {
                return String();
            }
            if (nameAvailable(candidate)) return candidate;
        }
        return String();
    };
    const auto sameApplication = [](const V1Profile& lhs, const V1Profile& rhs) {
        return lhs.detector == rhs.detector &&
               memcmp(lhs.settings.bytes, rhs.settings.bytes, sizeof(lhs.settings.bytes)) == 0;
    };

    String assignedNames[3];
    V1Settings legacySettings;
    if (!copySettingsChecked(settings_, legacySettings)) return false;
    const V1Settings& legacySettingsView = legacySettings;
    for (int slotIndex = 0; slotIndex < 3; ++slotIndex) {
        const V1Settings::ConstAutoPushSlotView slot = legacySettingsView.autoPushSlotView(slotIndex);
        const V1Profile* source = findLegacy(slot.config.profileName);
        V1Profile effective = source ? *source : V1Profile();
        if (source && !profileStringFieldsEqual(*source, effective)) return false;
        effective.schemaVersion = V1_PROFILE_SCHEMA_VERSION;
        effective.detector = V1DetectorConfiguration{};
        effective.detector.userSettingsPolicy = source ? V1UserSettingsPolicy::Value
                                                       : V1UserSettingsPolicy::Unchanged;
        if (source) {
            // This is the exact legacy applySlotMuteToZero transform.
            if (slot.muteToZero) {
                effective.settings.bytes[0] &= static_cast<uint8_t>(~0x10u);
            } else {
                effective.settings.bytes[0] |= 0x10u;
            }
        }
        if (slot.config.mode != V1_MODE_UNKNOWN) {
            effective.detector.modePolicy = V1ModePolicy::Value;
            effective.detector.mode = static_cast<uint8_t>(slot.config.mode);
        }
        // Legacy Auto-Push always sent one display command, including for an
        // otherwise-empty slot. Off means the legacy completely-dark command.
        effective.detector.displayPolicy = slot.darkMode ? V1DisplayPolicy::Off : V1DisplayPolicy::On;
        if (slot.darkMode) effective.detector.bluetoothLedPolicy = V1BluetoothLedPolicy::Off;
        if (isConfiguredSlotVolumePair(slot.volume, slot.muteVolume)) {
            effective.detector.volumePolicy = V1VolumePolicy::Temporary;
            effective.detector.mainVolume = slot.volume;
            effective.detector.mutedVolume = slot.muteVolume;
        }

        String sourceKey;
        if (source && !exactStringCopy(source->name, sourceKey)) return false;
        bool reused = false;
        for (size_t variantIndex = 0; variantIndex < variants.size(); ++variantIndex) {
            const Variant& variant = variants[variantIndex];
            if (variant.sourceName == sourceKey && sameApplication(variant.profile, effective)) {
                if (!exactStringCopy(variant.effectiveName, assignedNames[slotIndex])) return false;
                reused = true;
                break;
            }
        }
        if (reused) continue;

        String effectiveName;
        const bool firstSourceVariant = [&]() {
            for (size_t variantIndex = 0; variantIndex < variants.size(); ++variantIndex) {
                const Variant& variant = variants[variantIndex];
                if (variant.sourceName == sourceKey) return false;
            }
            return true;
        }();
        if (source && firstSourceVariant) {
            if (!exactStringCopy(source->name, effectiveName)) return false;
            const int index = catalogIndex(effectiveName);
            if (index < 0) return false;
            effective.name = effectiveName;
            if (effective.name.length() != effectiveName.length() || effective.name != effectiveName) return false;
            catalog[static_cast<size_t>(index)] = effective;
            if (!profileStringFieldsEqual(catalog[static_cast<size_t>(index)], effective)) return false;
        } else {
            // Slot labels intentionally allow display characters that profile
            // names reject. Use a stable safe suffix rather than laundering a
            // legal label into an invalid filesystem name.
            char safeSlotBytes[8];
            const int safeSlotLength = std::snprintf(safeSlotBytes, sizeof(safeSlotBytes), "Slot %d", slotIndex + 1);
            if (safeSlotLength <= 0 || static_cast<size_t>(safeSlotLength) >= sizeof(safeSlotBytes)) return false;
            String safeSlot(safeSlotBytes);
            if (safeSlot.length() != static_cast<size_t>(safeSlotLength)) return false;
            String stem = source ? source->name : String("Auto-Push");
            if (stem.length() != (source ? source->name.length() : std::strlen("Auto-Push"))) return false;
            String fixedSuffix = source ? String(" - ") : String(" ");
            if (fixedSuffix.length() != (source ? 3u : 1u)) return false;
            const size_t expectedSuffixLength = fixedSuffix.length() + safeSlot.length();
            fixedSuffix += safeSlot;
            if (fixedSuffix.length() != expectedSuffixLength) return false;
            effectiveName = uniqueName(stem, fixedSuffix.c_str());
            if (effectiveName.length() == 0) return false;
            effective.name = effectiveName;
            if (effective.name.length() != effectiveName.length() || effective.name != effectiveName) return false;
            if (!source) {
                effective.description = "Migrated Auto-Push detector configuration";
                if (effective.description.length() !=
                    std::strlen("Migrated Auto-Push detector configuration")) return false;
            }
            if (catalog.size() >= V1_PROFILE_CATALOG_MAX_COUNT) return false;
            catalog.push_back(effective);
            if (!profileStringFieldsEqual(catalog.back(), effective)) return false;
        }
        if (variants.size() >= 3) return false;
        variants.emplace_back();
        Variant& variant = variants.back();
        if (!exactStringCopy(sourceKey, variant.sourceName) ||
            !exactStringCopy(effectiveName, variant.effectiveName)) return false;
        variant.profile = effective;
        if (!profileStringFieldsEqual(variant.profile, effective)) return false;
        if (variant.sourceName != sourceKey || variant.effectiveName != effectiveName ||
            !profileStringFieldsEqual(variant.profile, effective)) return false;
        if (!exactStringCopy(effectiveName, assignedNames[slotIndex])) return false;
    }

    PsramJson::Document migration;
    migration["autoPushEnabled"] = settings_.autoPushEnabled;
    migration["activeSlot"] = settings_.activeSlot;
    migration["autoPushProfileSchemaVersion"] = V1_PROFILE_SCHEMA_VERSION;
    for (int slotIndex = 0; slotIndex < 3; ++slotIndex) {
        char key[32];
        std::snprintf(key, sizeof(key), "slot%dProfileName", slotIndex);
        migration[key] = assignedNames[slotIndex];
        std::snprintf(key, sizeof(key), "slot%dMode", slotIndex);
        migration[key] = 0;
        std::snprintf(key, sizeof(key), "slot%dVolume", slotIndex);
        migration[key] = 255;
        std::snprintf(key, sizeof(key), "slot%dMuteVolume", slotIndex);
        migration[key] = 255;
        std::snprintf(key, sizeof(key), "slot%dDarkMode", slotIndex);
        migration[key] = false;
        std::snprintf(key, sizeof(key), "slot%dMuteToZero", slotIndex);
        migration[key] = false;
    }
    JsonArray profiles = migration["profiles"].to<JsonArray>();
    for (const V1Profile& profile : catalog) {
        JsonObject entry = profiles.add<JsonObject>();
        entry["schemaVersion"] = V1_PROFILE_SCHEMA_VERSION;
        entry["name"] = profile.name;
        entry["description"] = profile.description;
        appendV1DetectorConfiguration(entry["detector"].to<JsonObject>(), profile.detector);
        JsonArray bytes = entry["bytes"].to<JsonArray>();
        for (uint8_t byte : profile.settings.bytes) bytes.add(byte);
    }
    if (migration.overflowed()) return false;

#ifdef UNIT_TEST
    if (autoPushMigrationInterruptAfterProfiles_) {
        autoPushMigrationInterruptAfterProfiles_ = false;
        restoreInterruptAfterProfiles_ = true;
    }
#endif

    const SettingsBackupApplyResult result =
        applyBackupDocument(migration, true, SettingsRestoreWatchdog{}, SettingsBackupScope::ProfilesOnly);
    if (!result.success) {
        // The profile transaction may have staged the v2 document in RAM
        // before its durable authority commit. Keep the running executor on
        // the legacy command source until recovery proves that commit.
        settings_ = std::move(legacySettings);
        Serial.println("[Settings] Auto-Push profile schema migration did not commit; retaining legacy commands");
        return false;
    }
    Serial.printf("[Settings] Migrated Auto-Push detector ownership to %d profile(s)\n",
                  result.profilesRestored);
    return true;
}

bool backupFieldMatchesBool(const JsonDocument& doc, const char* key, bool expected) {
    bool parsed = false;
    return parseBoolVariant(doc[key], parsed) && parsed == expected;
}

bool backupFieldMatchesInt(const JsonDocument& doc, const char* key, int expected) {
    return doc[key].is<int>() && doc[key].as<int>() == expected;
}

bool backupFieldMatchesString(const JsonDocument& doc, const char* key, const String& expected) {
    if (!doc[key].is<const char*>()) return false;
    const JsonString actual = doc[key].as<JsonString>();
    return actual.c_str() && actual.size() == expected.length() &&
           (actual.size() == 0 || std::memcmp(actual.c_str(), expected.c_str(), actual.size()) == 0);
}

bool backupAppearsInSyncWithNvs(const JsonDocument& doc, const V1Settings& current) {
    // Core fields that should track one-for-one between healthy NVS and SD backup.
    return backupFieldMatchesBool(doc, "wifiClientEnabled", current.wifiClientEnabled) &&
           backupFieldMatchesString(doc, "wifiClientSSID", current.wifiClientSSID) &&
           backupFieldMatchesBool(doc, "proxyBLE", current.proxyBLE) &&
           backupFieldMatchesString(doc, "proxyName", current.proxyName) &&
           backupFieldMatchesInt(doc, "brightness", current.brightness) &&
           backupFieldMatchesBool(doc, "autoPushEnabled", current.autoPushEnabled) &&
           backupFieldMatchesInt(doc, "autoPushProfileSchemaVersion",
                                 current.autoPushProfileSchemaVersion) &&
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
