#include <cstdio>
#include "backup_payload_builder.h"
#include "display_visual_contract.h"
#include "json_exact_input.h"
#include "settings_internals.h"
#include "settings_sanitize.h"

#include <ArduinoJson.h>
#include <cctype>
#include <cstring>

namespace BackupPayloadBuilder {

namespace {

constexpr const char* HTTP_BACKUP_TYPE = "v1simple_backup";
constexpr const char* SD_BACKUP_TYPE = "v1simple_sd_backup";
// Early downloadable backups used this name. They remain import-compatible;
// new exports continue to use HTTP_BACKUP_TYPE.
constexpr const char* LEGACY_HTTP_BACKUP_TYPE = "v1simple_http_backup";
// computeCrc32 is the canonical IEEE 802.3 CRC32 from settings_backup.cpp.

class Crc32Writer {
  public:
    size_t write(uint8_t byte) {
        crc_ ^= byte;
        for (uint8_t bit = 0; bit < 8; ++bit) {
            crc_ = (crc_ >> 1u) ^ ((crc_ & 1u) ? 0xEDB88320u : 0u);
        }
        return 1;
    }

    size_t write(const uint8_t* data, size_t length) {
        for (size_t index = 0; index < length; ++index) write(data[index]);
        return length;
    }

    uint32_t value() const { return crc_ ^ 0xFFFFFFFFu; }

  private:
    uint32_t crc_ = 0xFFFFFFFFu;
};

bool isBackupCrcKey(JsonString key) {
    static constexpr char CRC_KEY[] = "_crc32";
    return key.size() == sizeof(CRC_KEY) - 1u && memcmp(key.c_str(), CRC_KEY, sizeof(CRC_KEY) - 1u) == 0;
}

bool serializableText(const String& value, size_t maxBytes) {
    return value.length() <= maxBytes && std::strlen(value.c_str()) == value.length() &&
           ExactJsonInput::validSemanticString(value.c_str(), value.length());
}

bool isTrimmedText(const String& value) {
    return value.length() == 0 ||
           (!std::isspace(static_cast<unsigned char>(value[0])) &&
            !std::isspace(static_cast<unsigned char>(value[value.length() - 1u])));
}

bool isCanonicalLastV1Address(const String& value) {
    if (!isTrimmedText(value)) return false;
    for (size_t index = 0; index < value.length(); ++index) {
        const uint8_t byte = static_cast<uint8_t>(value[index]);
        if (byte >= static_cast<uint8_t>('a') && byte <= static_cast<uint8_t>('z')) return false;
    }
    return true;
}

bool settingsTextIsSerializable(const V1Settings& settings) {
    if (!serializableText(settings.apSSID, MAX_WIFI_SSID_LEN) ||
        !serializableText(settings.wifiClientSSID, MAX_WIFI_SSID_LEN) ||
        !serializableText(settings.proxyName, MAX_PROXY_NAME_LEN) ||
        !serializableText(settings.lastV1Address, 17) ||
        !serializableText(settings.obdSavedAddress, 17) ||
        !serializableText(settings.obdSavedName, 32) ||
        !serializableText(settings.slot0Name, MAX_SLOT_NAME_LEN) ||
        !serializableText(settings.slot1Name, MAX_SLOT_NAME_LEN) ||
        !serializableText(settings.slot2Name, MAX_SLOT_NAME_LEN)) {
        return false;
    }
    if (settings.apSSID.length() == 0 || settings.proxyName.length() == 0 ||
        !isCanonicalLastV1Address(settings.lastV1Address) ||
        !isValidBleAddress(settings.obdSavedAddress) ||
        !isTrimmedText(settings.obdSavedName) ||
        !isCanonicalSlotNameValue(settings.slot0Name) ||
        !isCanonicalSlotNameValue(settings.slot1Name) ||
        !isCanonicalSlotNameValue(settings.slot2Name)) {
        return false;
    }
    for (size_t index = 0; index < kWifiStaSlotCount; ++index) {
        if (!serializableText(settings.wifiStaSlots[index].ssid, MAX_WIFI_SSID_LEN) ||
            !serializableText(settings.wifiStaSlots[index].label, MAX_WIFI_STA_LABEL_LEN) ||
            !isTrimmedText(settings.wifiStaSlots[index].label)) {
            return false;
        }
    }
    for (uint8_t slot = 0; slot < 3; ++slot) {
        const String& profileName = settings.autoPushSlotView(slot).config.profileName;
        if (!serializableText(profileName, MAX_PROFILE_NAME_LEN)) return false;
        if (profileName.length() > 0) {
            String canonical;
            if (canonicalizeProfileName(profileName, canonical) != ProfileNameStatus::Valid ||
                canonical != profileName) {
                return false;
            }
        }
    }
    return true;
}

bool settingsCurrentBackupStateIsCanonical(const V1Settings& settings) {
    if (settings.proxyBLE && settings.obdEnabled) return false;
    // Exact backups are rebuild-stable. Legacy slot-owned
    // detector state must complete its explicit migration before it can be
    // represented without canonicalization or command drift.
    if (settings.autoPushProfileSchemaVersion != V1_PROFILE_SCHEMA_VERSION) return false;

    bool canonicalColors = true;
    DisplaySettingsContract::forEachColor([&](const auto& field) {
        if (settings.*(field.value) == 0) canonicalColors = false;
    });
    if (!canonicalColors || settings.slot0Color == 0 || settings.slot1Color == 0 || settings.slot2Color == 0) return false;
    for (uint16_t color : settings.colorBars) {
        if (color == 0) return false;
    }

    for (size_t index = 0; index < kWifiStaSlotCount; ++index) {
        const WifiStaSlot& slot = settings.wifiStaSlots[index];
        if (slot.isConfigured() && slot.label.length() == 0) return false;
        if (!slot.isConfigured() &&
            (slot.label.length() != 0 || slot.priority != 0 || slot.lastConnectedAtSec != 0)) {
            return false;
        }
    }
    for (uint8_t slotIndex = 0; slotIndex < 3; ++slotIndex) {
        const auto slot = settings.autoPushSlotView(slotIndex);
        if (slot.config.mode != V1_MODE_UNKNOWN ||
            (slot.volumeOverride ? slot.volume > 9 || slot.muteVolume > 9
                                 : slot.volume != 0xFF || slot.muteVolume != 0xFF) ||
            (slot.darkMode && !slot.darkModeOverride) || slot.muteToZero) return false;
    }
    return true;
}

void writeJsonString(Crc32Writer& writer, JsonString value) {
    writer.write(static_cast<uint8_t>('"'));
    for (size_t index = 0; index < value.size(); ++index) {
        const uint8_t byte = static_cast<uint8_t>(value.c_str()[index]);
        const char escaped = byte == '"'    ? '"'
                             : byte == '\\' ? '\\'
                             : byte == '\b' ? 'b'
                             : byte == '\f' ? 'f'
                             : byte == '\n' ? 'n'
                             : byte == '\r' ? 'r'
                             : byte == '\t' ? 't'
                                            : 0;
        if (escaped != 0) {
            writer.write(static_cast<uint8_t>('\\'));
            writer.write(static_cast<uint8_t>(escaped));
        } else if (byte == 0) {
            static constexpr uint8_t ZERO_ESCAPE[] = {'\\', 'u', '0', '0', '0', '0'};
            writer.write(ZERO_ESCAPE, sizeof(ZERO_ESCAPE));
        } else {
            writer.write(byte);
        }
    }
    writer.write(static_cast<uint8_t>('"'));
}

void appendProfile(JsonArray profilesArr, const V1Profile& profile, bool profileOwned) {
    JsonObject p = profilesArr.add<JsonObject>();
    p["name"] = profile.name;
    p["description"] = profile.description;
    if (profileOwned) {
        p["schemaVersion"] = V1_PROFILE_SCHEMA_VERSION;
        appendV1DetectorConfiguration(p["detector"].to<JsonObject>(), profile.detector);
        appendV1InTheBoxSettings(p["inTheBox"].to<JsonObject>(), profile.inTheBox);
    } else {
        // Until the dedicated ownership marker commits, slots remain the
        // authoritative detector-command source. Keep the pre-v2 profile
        // representation so a backup cannot claim a half-migrated catalog.
        p["displayOn"] = profile.displayOn;
        p["mainVolume"] = profile.mainVolume;
        p["mutedVolume"] = profile.mutedVolume;
    }

    JsonArray bytes = p["bytes"].to<JsonArray>();
    for (int i = 0; i < 6; i++) {
        bytes.add(profile.settings.bytes[i]);
    }
}

} // namespace

const char* backupTypeForTransport(BackupTransport transport) {
    return transport == BackupTransport::SdBackup ? SD_BACKUP_TYPE : HTTP_BACKUP_TYPE;
}

bool isRecognizedBackupType(const char* type) {
    if (type == nullptr) {
        return false;
    }
    return strcmp(type, HTTP_BACKUP_TYPE) == 0 || strcmp(type, SD_BACKUP_TYPE) == 0 ||
           strcmp(type, LEGACY_HTTP_BACKUP_TYPE) == 0;
}

bool isRecognizedBackupType(JsonVariantConst type) {
    return exactV1JsonToken(type, HTTP_BACKUP_TYPE) || exactV1JsonToken(type, SD_BACKUP_TYPE) ||
           exactV1JsonToken(type, LEGACY_HTTP_BACKUP_TYPE);
}

BuildResult buildBackupDocument(JsonDocument& doc, const V1Settings& settings, const V1ProfileManager& profileManager,
                                BackupTransport transport, uint32_t snapshotMs) {
    doc.clear();
    BuildResult result;

    // Settings can predate the strict HTTP/form boundaries or originate from
    // station discovery. Never turn an unsafe persisted byte sequence into an
    // invalid JSON response, cached HTTP snapshot, or replacement SD backup.
    if (!settingsTextIsSerializable(settings) ||
        !settingsCurrentBackupStateIsCanonical(settings)) {
        result.safeToCommit = false;
        return result;
    }

    doc["_type"] = backupTypeForTransport(transport);
    doc["_version"] = SD_BACKUP_VERSION;
    doc["_timestamp"] = snapshotMs;
    doc["timestamp"] = snapshotMs;

    doc["apSSID"] = settings.apSSID;
    // Credentials are recovery data, not network-downloadable settings.
    // Keep the AP password only in the local SD backup. HTTP downloads omit it,
    // and applyBackupDocument already preserves an existing password when the
    // key is absent.
    if (transport == BackupTransport::SdBackup) {
        if (!serializableText(settings.apPassword, MAX_AP_PASSWORD_LEN) ||
            settings.apPassword.length() < MIN_AP_PASSWORD_LEN) {
            result.safeToCommit = false;
            return result;
        }
        String encodedPassword;
        if (!encodeObfuscatedForStorage(settings.apPassword, encodedPassword)) {
            result.safeToCommit = false;
            return result;
        }
        doc["apPassword"] = encodedPassword;
    }
    doc["wifiClientEnabled"] = settings.wifiClientEnabled;
    const WifiStaSlot* primaryStaSlot = settings.primaryWifiStaSlot();
    const String& backupWifiClientSsid =
        primaryStaSlot ? primaryStaSlot->ssid : settings.wifiClientSSID;
    doc["wifiClientSSID"] = backupWifiClientSsid;
    JsonArray staSlots = doc["wifiStaSlots"].to<JsonArray>();
    bool wroteStaSlot = false;
    for (size_t i = 0; i < kWifiStaSlotCount; ++i) {
        const WifiStaSlot& slot = settings.wifiStaSlots[i];
        if (!slot.isConfigured() && slot.label.length() == 0 && slot.lastConnectedAtSec == 0 && slot.priority == 0) {
            continue;
        }
        JsonObject slotObj = staSlots.add<JsonObject>();
        slotObj["index"] = static_cast<uint8_t>(i);
        slotObj["ssid"] = slot.ssid;
        slotObj["label"] = slot.label;
        slotObj["priority"] = slot.priority;
        // Keep the legacy JSON key for existing v21 backup readers. The value
        // is a logical recency token, not elapsed or wall-clock seconds.
        slotObj["lastConnectedAtSec"] = slot.lastConnectedAtSec;
        wroteStaSlot = true;
    }
    if (!wroteStaSlot && backupWifiClientSsid.length() > 0) {
        JsonObject slotObj = staSlots.add<JsonObject>();
        slotObj["index"] = 0;
        slotObj["ssid"] = backupWifiClientSsid;
        slotObj["label"] = "Saved";
        slotObj["priority"] = 0;
        slotObj["lastConnectedAtSec"] = 0;
    }
    doc["proxyBLE"] = settings.proxyBLE;
    doc["proxyName"] = settings.proxyName;
    doc["lastV1Address"] = settings.lastV1Address;
    doc["autoPowerOffMinutes"] = settings.autoPowerOffMinutes;
    doc["apTimeoutMinutes"] = settings.apTimeoutMinutes;

    // OBD settings
    doc["obdEnabled"] = settings.obdEnabled;
    doc["obdSavedAddress"] = settings.obdSavedAddress;
    doc["obdSavedName"] = settings.obdSavedName;
    doc["obdSavedAddrType"] = settings.obdSavedAddrType;
    doc["obdMinRssi"] = settings.obdMinRssi;
    doc["obdScanWindowMs"] = settings.obdScanWindowMs;
    doc["obdRetryIntervalMs"] = settings.obdRetryIntervalMs;
    doc["proxyOpenWindowMs"] = settings.proxyOpenWindowMs;
    doc["v1SettleQuietMs"] = settings.v1SettleQuietMs;
    doc["v1SettleFallbackMs"] = settings.v1SettleFallbackMs;
    doc["cycleTeardownAckTimeoutMs"] = settings.cycleTeardownAckTimeoutMs;

    // ALP settings
    doc["alpEnabled"] = settings.alpEnabled;
    doc["alpAlertPersistSec"] = settings.alpAlertPersistSec;
    doc["alpDisableV1LaserOnPush"] = settings.alpDisableV1LaserOnPush;

    // GPS settings
    doc["gpsEnabled"] = settings.gpsEnabled;
    doc["gpsBaud"] = settings.gpsBaud;
    // Descriptor names have static lifetime, preserving literal-key storage.
    const auto& brightness = DisplaySettingsContract::kBrightness;
    doc[JsonString(brightness.backupKey, true)] = settings.*(brightness.value);
    const auto writeColor = [&](const auto& field) {
        if (field.legacyBackupKey) doc[JsonString(field.legacyBackupKey, true)] = settings.*(field.value);
        doc[JsonString(field.backupKey, true)] = settings.*(field.value);
    };
    for (const auto& field : DisplaySettingsContract::kColorsBeforeBars) writeColor(field);
    // Six physical-segment colours are authoritative in v19 backups.
    for (int barIndex = 0; barIndex < 6; ++barIndex) {
        doc[JsonString(DisplaySettingsContract::kBars[barIndex].backupKey, true)] = settings.colorBars[barIndex];
    }
    // Keep an expanded v11-shaped shadow so older firmware can consume a new
    // backup without losing the theme.
    uint16_t compatibilitySegments[8];
    DisplayVisualContract::expandSixBarColorsToEight(settings.colorBars, compatibilitySegments);
    for (int barIndex = 0; barIndex < 8; ++barIndex) {
        doc[JsonString(DisplaySettingsContract::kLegacyBars[barIndex].backupKey, true)] = compatibilitySegments[barIndex];
    }
    for (const auto& field : DisplaySettingsContract::kColorsAfterBars) writeColor(field);
    for (const auto& field : DisplaySettingsContract::kFlags) {
        doc[JsonString(field.backupKey, true)] = settings.*(field.value);
    }

    // Voice alert settings
    doc["voiceAlertMode"] = static_cast<int>(settings.voiceAlertMode);
    doc["voiceDirectionEnabled"] = settings.voiceDirectionEnabled;
    doc["announceBogeyCount"] = settings.announceBogeyCount;
    doc["muteVoiceIfVolZero"] = settings.muteVoiceIfVolZero;
    doc["voiceVolume"] = settings.voiceVolume;
    doc["announceSecondaryAlerts"] = settings.announceSecondaryAlerts;
    doc["secondaryLaser"] = settings.secondaryLaser;
    doc["secondaryKa"] = settings.secondaryKa;
    doc["secondaryK"] = settings.secondaryK;
    doc["secondaryX"] = settings.secondaryX;

    doc["alertVolumeFadeEnabled"] = settings.alertVolumeFadeEnabled;
    doc["alertVolumeFadeDelaySec"] = settings.alertVolumeFadeDelaySec;
    doc["alertVolumeFadeVolume"] = settings.alertVolumeFadeVolume;
    doc["speedMuteEnabled"] = settings.speedMuteEnabled;
    doc["speedMuteThresholdMph"] = settings.speedMuteThresholdMph;
    doc["speedMuteHysteresisMph"] = settings.speedMuteHysteresisMph;
    doc["speedMuteVolume"] = settings.speedMuteVolume;
    doc["speedMuteVoice"] = settings.speedMuteVoice;
    doc["stealthEnabled"] = settings.stealthEnabled;

    doc["autoPushEnabled"] = settings.autoPushEnabled;
    // Current backups encode only durable ownership states. A schema-v2
    // catalog must complete its transactional migration before it can be
    // exported as a canonical current document.
    const uint8_t exportedProfileSchemaVersion = settings.autoPushProfileSchemaVersion;
    doc["autoPushProfileSchemaVersion"] = exportedProfileSchemaVersion;
    doc["activeSlot"] = settings.activeSlot;
    for (int index = 0; index < 3; ++index) {
        const auto slot = settings.autoPushSlotView(index);
        char key[24];
        std::snprintf(key, sizeof(key), "slot%dName", index);
        doc[key] = slot.name;
        std::snprintf(key, sizeof(key), "slot%dColor", index);
        doc[key] = slot.color;
        std::snprintf(key, sizeof(key), "slot%dVolume", index);
        doc[key] = slot.volume;
        std::snprintf(key, sizeof(key), "slot%dMuteVolume", index);
        doc[key] = slot.muteVolume;
        std::snprintf(key, sizeof(key), "slot%dDarkMode", index);
        doc[key] = slot.darkMode;
        std::snprintf(key, sizeof(key), "slot%dVolumeOverride", index);
        doc[key] = slot.volumeOverride;
        std::snprintf(key, sizeof(key), "slot%dDarkModeOverride", index);
        doc[key] = slot.darkModeOverride;
        std::snprintf(key, sizeof(key), "slot%dMuteToZero", index);
        doc[key] = slot.muteToZero;
        std::snprintf(key, sizeof(key), "slot%dAlertPersist", index);
        doc[key] = slot.alertPersist;
        std::snprintf(key, sizeof(key), "slot%dPriorityArrow", index);
        doc[key] = slot.priorityArrow;
        std::snprintf(key, sizeof(key), "slot%dProfileName", index);
        doc[key] = slot.config.profileName;
        std::snprintf(key, sizeof(key), "slot%dMode", index);
        doc[key] = slot.config.mode;
    }

    JsonArray profilesArr = doc["profiles"].to<JsonArray>();
    if (profileManager.isReady()) {
        std::vector<V1Profile> profileSnapshot;
        const ProfileOperationResult snapshotResult = profileManager.snapshotProfiles(profileSnapshot);
        result.profileStatus = snapshotResult.status;
        result.profileCatalogGenuinelyEmpty = snapshotResult.success() && profileSnapshot.empty();
        const bool profileOwned = exportedProfileSchemaVersion == V1_PROFILE_SCHEMA_VERSION;
        for (const V1Profile& profile : profileSnapshot) {
            String canonical;
            if (!serializableText(profile.name, MAX_PROFILE_NAME_LEN) ||
                canonicalizeProfileName(profile.name, canonical) != ProfileNameStatus::Valid ||
                canonical != profile.name ||
                !serializableText(profile.description, V1_PROFILE_DESCRIPTION_MAX_BYTES) ||
                !validV1ProfileDescription(profile.description) ||
                (profileOwned && profile.schemaVersion != V1_PROFILE_SCHEMA_VERSION)) {
                result.safeToCommit = false;
                continue;
            }
            appendProfile(profilesArr, profile, profileOwned);
            result.profilesBackedUp++;
        }
        result.safeToCommit = result.safeToCommit && snapshotResult.success();
        for (int slot = 0; slot < 3; ++slot) {
            const String& assigned = settings.autoPushSlotView(slot).config.profileName;
            if (assigned.length() == 0) continue;
            String canonical;
            bool found = false;
            if (canonicalizeProfileName(assigned, canonical) == ProfileNameStatus::Valid) {
                for (const V1Profile& profile : profileSnapshot) found |= profile.name == canonical;
            }
            // An unrelated survivor must not let an incomplete catalog replace
            // the last backup containing a configured profile.
            if (!found) result.safeToCommit = false;
        }
    } else {
        result.profileStatus = ProfileStorageStatus::Busy;
        result.profileCatalogGenuinelyEmpty = false;
        result.safeToCommit = false;
    }

    // Stamp _crc32 on SD backups to catch media-level corruption. The checksum
    // is part of the local SD recovery format; HTTP downloads omit this field.
    // The hash covers the complete document *before* _crc32 is appended.
    if (transport == BackupTransport::SdBackup) {
        doc["_crc32"] = computeBackupCrc32(doc);
    }

    if (doc.overflowed() || !validateCurrentBackupDocumentShape(doc)) {
        result.safeToCommit = false;
    }

    return result;
}

uint32_t computeBackupCrc32(const JsonDocument& doc) {
    // Hash the exact minified top-level object with _crc32 omitted. Streaming
    // avoids a second JsonDocument and a complete String copy for 128 KiB
    // restore and SD-backup documents.
    Crc32Writer writer;
    if (!doc.is<JsonObjectConst>()) {
        serializeJson(doc, writer);
        return writer.value();
    }

    writer.write(static_cast<uint8_t>('{'));
    bool first = true;
    for (JsonPairConst pair : doc.as<JsonObjectConst>()) {
        if (isBackupCrcKey(pair.key())) continue;
        if (!first) writer.write(static_cast<uint8_t>(','));
        first = false;
        writeJsonString(writer, pair.key());
        writer.write(static_cast<uint8_t>(':'));
        serializeJson(pair.value(), writer);
    }
    writer.write(static_cast<uint8_t>('}'));
    return writer.value();
}

} // namespace BackupPayloadBuilder
