/**
 * V1 Profile Manager Implementation
 */

#include "v1_profiles.h"
#include "backup_payload_builder.h"
#include "json_exact_input.h"
#include "psram_json_document.h"
#include "storage_manager.h"
#include "v1_settings_json.h"
#include <ArduinoJson.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <utility>
#include <vector>

// Shared CRC32 from settings_backup.cpp (canonical IEEE 802.3 table, check value 0xCBF43926).
extern uint32_t computeCrc32(const uint8_t* data, size_t length);

uint32_t V1ProfileManager::calculateCRC32(const uint8_t* data, size_t length) {
    return computeCrc32(data, length);
}

namespace {

bool detectorConfigurationCrc(const V1DetectorConfiguration& config, uint32_t& out,
                              uint8_t schemaVersion = V1_PROFILE_SCHEMA_VERSION);

bool profileDocumentCrc(JsonDocument& doc, uint32_t& out) {
    const JsonVariantConst stored = doc["profileCrc32"];
    const bool restore = !stored.isUnbound();
    const uint32_t storedValue = stored.is<uint32_t>() ? stored.as<uint32_t>() : 0;
    doc.remove("profileCrc32");
    if (doc.overflowed()) return false;
    out = BackupPayloadBuilder::computeBackupCrc32(doc);
    if (restore) doc["profileCrc32"] = storedValue;
    return !doc.overflowed();
}

class ProfileStorageGuard {
  public:
    ProfileStorageGuard(const V1ProfileManager&, StorageManager* storage, bool usingSd, uint32_t timeoutMs)
        : mutex_(usingSd && storage ? storage->getSDMutex() : nullptr), acquired_(mutex_ == nullptr) {
        if (mutex_) {
            acquired_ = xSemaphoreTake(mutex_, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
        }
    }
    ~ProfileStorageGuard() {
        if (acquired_ && mutex_) {
            xSemaphoreGive(mutex_);
        }
    }
    bool acquired() const { return acquired_; }

  private:
    SemaphoreHandle_t mutex_;
    bool acquired_;
};

ProfileOperationResult profileResult(ProfileStorageStatus status, const String& error = "") {
    ProfileOperationResult result;
    result.status = status;
    result.error = error;
    return result;
}

constexpr const char* kBooleanSettingFields[] = {
    "xBand",          "kBand",           "kaBand",          "laser",
    "kuBand",         "euro",            "kVerifier",       "laserRear",
    "customFreqs",    "kaAlwaysPriority", "fastLaserDetect", "muteToMuteVolume",
    "bogeyLockLoud",  "muteXKRear",       "startupSequence", "restingDisplay",
    "bsmPlus",         "mrct",             "driveSafe3D",      "driveSafe3DHD",
    "redflexHalo",     "redflexNK7",       "ekin",             "photoVerifier",
    "gatsoRT4",        "photoIntersectionFilter",
};

constexpr const char* kEnumSettingFields[] = {
    "kaSensitivity",
    "kSensitivity",
    "xSensitivity",
    "autoMute",
};

bool settingKeyEquals(JsonString key, const char* expected) {
    const size_t expectedLength = std::strlen(expected);
    return key.c_str() && key.size() == expectedLength &&
           std::memcmp(key.c_str(), expected, expectedLength) == 0;
}

bool knownUserSettingKey(JsonString key) {
    if (settingKeyEquals(key, "bytes") || settingKeyEquals(key, "baseBytes")) return true;
    for (const char* name : kBooleanSettingFields) {
        if (settingKeyEquals(key, name)) return true;
    }
    for (const char* name : kEnumSettingFields) {
        if (settingKeyEquals(key, name)) return true;
    }
    return false;
}

bool validateHumanReadableSettings(const JsonObjectConst& settingsObj) {
    for (JsonPairConst pair : settingsObj) {
        if (!knownUserSettingKey(pair.key())) {
            Serial.println("[V1Profiles] Unknown user setting");
            return false;
        }
    }
    for (const char* name : kBooleanSettingFields) {
        const JsonVariantConst value = settingsObj[name];
        if (!value.isUnbound() && !value.is<bool>()) {
            Serial.printf("[V1Profiles] Setting '%s' must be a boolean\n", name);
            return false;
        }
    }

    for (const char* name : kEnumSettingFields) {
        const JsonVariantConst value = settingsObj[name];
        if (value.isUnbound()) {
            continue;
        }
        if (!value.is<uint8_t>()) {
            Serial.printf("[V1Profiles] Setting '%s' must be an integer from 1 to 3\n", name);
            return false;
        }
        const uint8_t parsed = value.as<uint8_t>();
        if (parsed < 1 || parsed > 3) {
            Serial.printf("[V1Profiles] Setting '%s' must be from 1 to 3\n", name);
            return false;
        }
    }

    return true;
}

void applyHumanReadableSettings(const JsonObjectConst& settingsObj, V1UserSettings& parsed,
                                bool& anyField);

bool legacyProfileKeyIsKnown(JsonString key) {
    return settingKeyEquals(key, "name") || settingKeyEquals(key, "description") ||
           settingKeyEquals(key, "displayOn") || settingKeyEquals(key, "mainVolume") ||
           settingKeyEquals(key, "mutedVolume") || settingKeyEquals(key, "bytes") ||
           settingKeyEquals(key, "crc32") ||
           (knownUserSettingKey(key) && !settingKeyEquals(key, "baseBytes"));
}

bool parseLegacyProfileSettings(const JsonObjectConst& source, V1UserSettings& settings,
                                bool& displayOn, uint8_t& mainVolume, uint8_t& mutedVolume) {
    for (JsonPairConst pair : source) {
        if (!legacyProfileKeyIsKnown(pair.key())) return false;
    }
    if (!source["displayOn"].is<bool>() || !source["mainVolume"].is<int>() ||
        !source["mutedVolume"].is<int>()) return false;
    const int main = source["mainVolume"].as<int>();
    const int muted = source["mutedVolume"].as<int>();
    if (!((main >= 0 && main <= 9) || main == 0xFF) ||
        !((muted >= 0 && muted <= 9) || muted == 0xFF)) return false;

    uint8_t raw[V1SettingsJson::kSettingsByteCount];
    if (!V1SettingsJson::parseRawBytes(source["bytes"], raw)) return false;
    if (!source["crc32"].isUnbound() &&
        (!source["crc32"].is<uint32_t>() ||
         source["crc32"].as<uint32_t>() != computeCrc32(raw, sizeof(raw)))) return false;

    // Persisted v1 writers emitted raw bytes plus optional human-readable
    // redundancy. Raw bytes remain authoritative, but any redundancy present
    // must be exact and agree instead of being silently coerced or ignored.
    for (const char* name : kBooleanSettingFields) {
        if (!source[name].isUnbound() && !source[name].is<bool>()) return false;
    }
    for (const char* name : kEnumSettingFields) {
        if (!source[name].isUnbound() &&
            (!source[name].is<uint8_t>() || source[name].as<uint8_t>() > 3)) return false;
    }

    V1UserSettings parsed;
    std::memcpy(parsed.bytes, raw, sizeof(raw));
    V1UserSettings redundant = parsed;
    bool anyHumanField = false;
    applyHumanReadableSettings(source, redundant, anyHumanField);
    if (anyHumanField && std::memcmp(parsed.bytes, redundant.bytes, sizeof(raw)) != 0) return false;

    settings = parsed;
    displayOn = source["displayOn"].as<bool>();
    mainVolume = static_cast<uint8_t>(main);
    mutedVolume = static_cast<uint8_t>(muted);
    return true;
}

bool versionedProfileKeyIsKnown(JsonString key) {
    return settingKeyEquals(key, "schemaVersion") || settingKeyEquals(key, "name") ||
           settingKeyEquals(key, "description") || settingKeyEquals(key, "detector") ||
           settingKeyEquals(key, "detectorCrc32") || settingKeyEquals(key, "bytes") ||
           settingKeyEquals(key, "crc32") || settingKeyEquals(key, "profileCrc32") ||
           (knownUserSettingKey(key) && !settingKeyEquals(key, "baseBytes"));
}

bool validateVersionedReadableSettings(const JsonObjectConst& source, const uint8_t* raw,
                                       bool requireCompleteReadableShape) {
    for (JsonPairConst pair : source) {
        if (!versionedProfileKeyIsKnown(pair.key())) return false;
    }
    for (const char* name : kBooleanSettingFields) {
        const JsonVariantConst value = source[name];
        if ((requireCompleteReadableShape && value.isUnbound()) ||
            (!value.isUnbound() && !value.is<bool>())) return false;
    }
    for (const char* name : kEnumSettingFields) {
        const JsonVariantConst value = source[name];
        if ((requireCompleteReadableShape && value.isUnbound()) ||
            (!value.isUnbound() &&
             (!value.is<uint8_t>() || value.as<uint8_t>() > 3))) return false;
    }
    V1UserSettings authoritative;
    std::memcpy(authoritative.bytes, raw, V1SettingsJson::kSettingsByteCount);
    V1UserSettings redundant = authoritative;
    bool anyReadable = false;
    applyHumanReadableSettings(source, redundant, anyReadable);
    return (!requireCompleteReadableShape || anyReadable) &&
           std::memcmp(authoritative.bytes, redundant.bytes, V1SettingsJson::kSettingsByteCount) == 0;
}

void applyHumanReadableSettings(const JsonObjectConst& settingsObj, V1UserSettings& parsed,
                                bool& anyField) {
#define APPLY_BOOL_FIELD(jsonName, setter)                 \
    do {                                                    \
        if (!settingsObj[jsonName].isUnbound()) {           \
            parsed.setter(settingsObj[jsonName].as<bool>()); \
            anyField = true;                                \
        }                                                   \
    } while (false)
#define APPLY_ENUM_FIELD(jsonName, setter)                    \
    do {                                                       \
        if (!settingsObj[jsonName].isUnbound()) {              \
            parsed.setter(settingsObj[jsonName].as<uint8_t>()); \
            anyField = true;                                   \
        }                                                      \
    } while (false)
    APPLY_BOOL_FIELD("xBand", setXBandEnabled);
    APPLY_BOOL_FIELD("kBand", setKBandEnabled);
    APPLY_BOOL_FIELD("kaBand", setKaBandEnabled);
    APPLY_BOOL_FIELD("laser", setLaserEnabled);
    APPLY_BOOL_FIELD("kuBand", setKuBandEnabled);
    APPLY_BOOL_FIELD("euro", setEuroMode);
    APPLY_BOOL_FIELD("kVerifier", setKVerifier);
    APPLY_BOOL_FIELD("laserRear", setLaserRear);
    APPLY_BOOL_FIELD("customFreqs", setCustomFreqs);
    APPLY_BOOL_FIELD("kaAlwaysPriority", setKaAlwaysPriority);
    APPLY_BOOL_FIELD("fastLaserDetect", setFastLaserDetect);
    APPLY_ENUM_FIELD("kaSensitivity", setKaSensitivity);
    APPLY_ENUM_FIELD("kSensitivity", setKSensitivity);
    APPLY_ENUM_FIELD("xSensitivity", setXSensitivity);
    APPLY_ENUM_FIELD("autoMute", setAutoMute);
    APPLY_BOOL_FIELD("muteToMuteVolume", setMuteToMuteVolume);
    APPLY_BOOL_FIELD("bogeyLockLoud", setBogeyLockLoud);
    APPLY_BOOL_FIELD("muteXKRear", setMuteXKRear);
    APPLY_BOOL_FIELD("startupSequence", setStartupSequence);
    APPLY_BOOL_FIELD("restingDisplay", setRestingDisplay);
    APPLY_BOOL_FIELD("bsmPlus", setBsmPlus);
    APPLY_BOOL_FIELD("mrct", setMrct);
    APPLY_BOOL_FIELD("driveSafe3D", setDriveSafe3D);
    APPLY_BOOL_FIELD("driveSafe3DHD", setDriveSafe3DHD);
    APPLY_BOOL_FIELD("redflexHalo", setRedflexHalo);
    APPLY_BOOL_FIELD("redflexNK7", setRedflexNK7);
    APPLY_BOOL_FIELD("ekin", setEkin);
    APPLY_BOOL_FIELD("photoVerifier", setPhotoVerifier);
    APPLY_BOOL_FIELD("gatsoRT4", setGatsoRT4);
    APPLY_BOOL_FIELD("photoIntersectionFilter", setPhotoIntersectionFilter);
#undef APPLY_ENUM_FIELD
#undef APPLY_BOOL_FIELD
}

struct ProfileSyncState {
    enum class Status : uint8_t {
        Absent,
        LegacyImplicit,
        LegacyMetadata,
        Current,
        Corrupt,
        Unavailable,
    };

    uint32_t version = 0;
    bool deleted = false;
    Status status = Status::Absent;
};

constexpr const char* PROFILE_SYNC_META_TYPE = "v1simple_profile_sync";
constexpr int PROFILE_SYNC_META_VERSION = 1;
constexpr size_t PROFILE_SYNC_META_MAX_BYTES = 512;

#ifdef UNIT_TEST
const char* g_failProfilePathSuffixForTest = nullptr;
#endif

struct ProfileFileInspection {
    bool exists = false;
    bool valid = false;
    bool unavailable = false;
    uint32_t contentCrc = 0;
};

bool appendPathSuffixChecked(const String& base, const char* suffix, String& output) {
    if (!suffix) return false;
#ifdef UNIT_TEST
    if (g_failProfilePathSuffixForTest &&
        std::strcmp(g_failProfilePathSuffixForTest, suffix) == 0) return false;
#endif
    const size_t suffixLength = std::strlen(suffix);
    const size_t expected = base.length() + suffixLength;
    String candidate;
    candidate.reserve(expected);
    candidate += base;
    candidate += suffix;
    if (candidate.length() != expected ||
        (base.length() != 0 && std::memcmp(candidate.c_str(), base.c_str(), base.length()) != 0) ||
        (suffixLength != 0 && std::memcmp(candidate.c_str() + base.length(), suffix, suffixLength) != 0)) {
        return false;
    }
    output = std::move(candidate);
    return output.length() == expected;
}

bool syncMetaPath(const String& profilePath, String& output) {
    return appendPathSuffixChecked(profilePath, ".meta", output);
}

bool syncStateCrc(const uint32_t version, const bool deleted, uint32_t& out) {
    // The metadata schema has four fixed canonical fields. Hash their exact
    // minified representation without a DOM clone or heap-backed String so a
    // low-memory tombstone/winner decision cannot accept a partial checksum.
    char canonical[192];
    const int length = snprintf(canonical, sizeof(canonical),
                                "{\"_type\":\"%s\",\"_version\":%d,\"version\":%lu,\"deleted\":%s}",
                                PROFILE_SYNC_META_TYPE, PROFILE_SYNC_META_VERSION,
                                static_cast<unsigned long>(version), deleted ? "true" : "false");
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(canonical)) return false;
    out = computeCrc32(reinterpret_cast<const uint8_t*>(canonical), static_cast<size_t>(length));
    return true;
}

bool parseSyncStateDocument(const JsonDocument& doc, ProfileSyncState& state) {
    JsonObjectConst object = doc.as<JsonObjectConst>();
    if (object.size() == 2 && doc["version"].is<uint32_t>() && doc["version"].as<uint32_t>() > 0 &&
        doc["deleted"].is<bool>()) {
        state.version = doc["version"].as<uint32_t>();
        state.deleted = doc["deleted"].as<bool>();
        state.status = ProfileSyncState::Status::LegacyMetadata;
        return true;
    }
    uint32_t computedCrc = 0;
    if (object.size() != 5 || !exactV1JsonToken(doc["_type"], PROFILE_SYNC_META_TYPE) ||
        !doc["_version"].is<int>() || doc["_version"].as<int>() != PROFILE_SYNC_META_VERSION ||
        !doc["version"].is<uint32_t>() || doc["version"].as<uint32_t>() == 0 ||
        !doc["deleted"].is<bool>() || !doc["_crc32"].is<uint32_t>() ||
        !syncStateCrc(doc["version"].as<uint32_t>(), doc["deleted"].as<bool>(), computedCrc) ||
        doc["_crc32"].as<uint32_t>() != computedCrc) {
        return false;
    }
    state.version = doc["version"].as<uint32_t>();
    state.deleted = doc["deleted"].as<bool>();
    state.status = ProfileSyncState::Status::Current;
    return true;
}

ProfileSyncState readSyncState(fs::FS& filesystem, const String& profilePath) {
    ProfileSyncState state;
    String metaPath;
    if (!syncMetaPath(profilePath, metaPath)) {
        state.status = ProfileSyncState::Status::Unavailable;
        return state;
    }
    if (!filesystem.exists(metaPath)) {
        if (filesystem.exists(profilePath)) {
            state.version = 1; // backward-compatible baseline for pre-metadata files
            state.deleted = false;
            state.status = ProfileSyncState::Status::LegacyImplicit;
        }
        return state;
    }

    File file = filesystem.open(metaPath, FILE_READ);
    if (!file) {
        state.status = ProfileSyncState::Status::Unavailable;
        return state;
    }
    if (file.size() == 0 || file.size() > PROFILE_SYNC_META_MAX_BYTES) {
        file.close();
        state.status = ProfileSyncState::Status::Corrupt;
        return state;
    }
    const size_t fileSize = file.size();
    PsramJson::Buffer bytes(fileSize);
    if (!bytes) {
        file.close();
        state.status = ProfileSyncState::Status::Unavailable;
        return state;
    }
    const size_t bytesRead = file.read(bytes.data(), fileSize);
    file.close();
    PsramJson::Document doc;
    if (bytesRead != fileSize) {
        state.status = ProfileSyncState::Status::Unavailable;
        return state;
    }
    const ExactJsonInput::Status exact = ExactJsonInput::validate(bytes.data(), bytes.size());
    if (exact == ExactJsonInput::Status::MemoryUnavailable) {
        state.status = ProfileSyncState::Status::Unavailable;
        return state;
    }
    const DeserializationError parseError =
        exact == ExactJsonInput::Status::Ok ? deserializeJson(doc, bytes.data(), bytes.size())
                                           : DeserializationError::InvalidInput;
    if (parseError == DeserializationError::NoMemory || doc.overflowed()) {
        state.status = ProfileSyncState::Status::Unavailable;
        return state;
    }
    if (exact != ExactJsonInput::Status::Ok || parseError || !parseSyncStateDocument(doc, state)) {
        state = ProfileSyncState{};
        state.status = ProfileSyncState::Status::Corrupt;
    }
    return state;
}

ProfileFileInspection inspectProfileFile(fs::FS& filesystem, const String& path, const String& expectedName) {
    ProfileFileInspection inspection;
    inspection.exists = filesystem.exists(path);
    if (!inspection.exists) return inspection;

    File file = filesystem.open(path, FILE_READ);
    if (!file) {
        inspection.unavailable = true;
        return inspection;
    }
    if (file.size() == 0 || file.size() > V1_PROFILE_FILE_MAX_BYTES) {
        if (file) file.close();
        return inspection;
    }

    const size_t fileSize = file.size();
    PsramJson::Buffer content(fileSize);
    if (!content) {
        file.close();
        inspection.unavailable = true;
        return inspection;
    }
    const size_t bytesRead = file.read(content.data(), fileSize);
    file.close();
    if (bytesRead != fileSize) {
        inspection.unavailable = true;
        return inspection;
    }

    PsramJson::Document doc;
    const ExactJsonInput::Status exact = ExactJsonInput::validate(content.data(), content.size());
    if (exact == ExactJsonInput::Status::MemoryUnavailable) {
        inspection.unavailable = true;
        return inspection;
    }
    if (exact != ExactJsonInput::Status::Ok) return inspection;
    const DeserializationError parseError = deserializeJson(doc, content.data(), content.size());
    if (parseError == DeserializationError::NoMemory || doc.overflowed()) {
        inspection.unavailable = true;
        return inspection;
    }
    if (parseError) return inspection;

    String serializedName;
    const ExactV1JsonStringStatus nameStatus =
        exactV1JsonStringChecked(doc["name"], serializedName, MAX_PROFILE_NAME_LEN);
    if (nameStatus == ExactV1JsonStringStatus::Unavailable) {
        inspection.unavailable = true;
        return inspection;
    }
    if (nameStatus != ExactV1JsonStringStatus::Valid || serializedName != expectedName) return inspection;

    String description;
    const ExactV1JsonStringStatus descriptionStatus =
        exactV1JsonStringChecked(doc["description"], description, V1_PROFILE_DESCRIPTION_MAX_BYTES);
    if (descriptionStatus == ExactV1JsonStringStatus::Unavailable) {
        inspection.unavailable = true;
        return inspection;
    }
    if (descriptionStatus != ExactV1JsonStringStatus::Valid) {
        return inspection;
    }

    const bool hasSchemaVersion = !doc["schemaVersion"].isUnbound();
    const bool hasDetector = !doc["detector"].isUnbound();
    const bool hasDetectorCrc = !doc["detectorCrc32"].isUnbound();
    if (hasSchemaVersion != hasDetector || hasSchemaVersion != hasDetectorCrc) return inspection;
    if (hasSchemaVersion) {
        V1DetectorConfiguration detector;
        uint32_t detectorCrc = 0;
        if (!doc["schemaVersion"].is<int>()) return inspection;
        const int schema = doc["schemaVersion"].as<int>();
        if ((schema != V1_PROFILE_PREVIOUS_SCHEMA_VERSION && schema != V1_PROFILE_SCHEMA_VERSION) ||
            !doc["detector"].is<JsonObjectConst>() ||
            !(schema == V1_PROFILE_SCHEMA_VERSION
                  ? parseV1DetectorConfiguration(doc["detector"].as<JsonObjectConst>(), detector)
                  : parseV1DetectorConfigurationV2(doc["detector"].as<JsonObjectConst>(), detector)) ||
            !doc["detectorCrc32"].is<uint32_t>()) {
            return inspection;
        }
        if (!detectorConfigurationCrc(detector, detectorCrc, static_cast<uint8_t>(schema))) {
            inspection.unavailable = true;
            return inspection;
        }
        if (doc["detectorCrc32"].as<uint32_t>() != detectorCrc) return inspection;
        const bool currentSchema = schema == V1_PROFILE_SCHEMA_VERSION;
        uint32_t profileCrc = 0;
        if (currentSchema != !doc["profileCrc32"].isUnbound() ||
            (currentSchema && (!doc["profileCrc32"].is<uint32_t>() ||
                               !profileDocumentCrc(doc, profileCrc) ||
                               doc["profileCrc32"].as<uint32_t>() != profileCrc))) return inspection;
    }

    const JsonVariantConst rawBytes = doc["bytes"];
    if (hasSchemaVersion && (rawBytes.isUnbound() || !doc["crc32"].is<uint32_t>())) return inspection;
    if (!hasSchemaVersion) {
        V1UserSettings legacySettings;
        bool displayOn = true;
        uint8_t mainVolume = 0xFF;
        uint8_t mutedVolume = 0xFF;
        if (!parseLegacyProfileSettings(doc.as<JsonObjectConst>(), legacySettings, displayOn,
                                        mainVolume, mutedVolume)) return inspection;
    } else if (!rawBytes.isUnbound()) {
        uint8_t parsed[V1SettingsJson::kSettingsByteCount];
        if (!V1SettingsJson::parseRawBytes(rawBytes, parsed)) return inspection;
        const int schema = doc["schemaVersion"].as<int>();
        if (!validateVersionedReadableSettings(doc.as<JsonObjectConst>(), parsed,
                                               schema == V1_PROFILE_SCHEMA_VERSION)) return inspection;
        if (doc["crc32"].is<uint32_t>() &&
            doc["crc32"].as<uint32_t>() !=
                computeCrc32(parsed, V1SettingsJson::kSettingsByteCount)) {
            return inspection;
        }
    }

    inspection.valid = true;
    inspection.contentCrc = computeCrc32(content.data(), content.size());
    return inspection;
}

bool writeSyncState(fs::FS& filesystem, const String& profilePath, const ProfileSyncState& state) {
    PsramJson::Document doc;
    doc["_type"] = PROFILE_SYNC_META_TYPE;
    doc["_version"] = PROFILE_SYNC_META_VERSION;
    doc["version"] = state.version;
    doc["deleted"] = state.deleted;
    uint32_t crc = 0;
    if (!syncStateCrc(state.version, state.deleted, crc)) return false;
    doc["_crc32"] = crc;
    String metaPath;
    String tmpPath;
    if (!syncMetaPath(profilePath, metaPath) ||
        !appendPathSuffixChecked(metaPath, ".tmp", tmpPath)) return false;
    File file = filesystem.open(tmpPath, FILE_WRITE);
    if (!file) return false;
    const size_t expected = measureJson(doc);
    const size_t written = serializeJson(doc, file);
    file.flush();
    file.close();
    if (written != expected) {
        filesystem.remove(tmpPath);
        return false;
    }

    File verify = filesystem.open(tmpPath, FILE_READ);
    const size_t verifySize = verify ? verify.size() : 0;
    PsramJson::Buffer verifyBytes(verifySize);
    PsramJson::Document verifiedDoc;
    ProfileSyncState verifiedState;
    const bool verified = verify && verifySize == written && verifyBytes &&
                          verify.read(verifyBytes.data(), verifySize) == verifySize &&
                          ExactJsonInput::validate(verifyBytes.data(), verifySize) == ExactJsonInput::Status::Ok &&
                          !deserializeJson(verifiedDoc, verifyBytes.data(), verifySize) && !verifiedDoc.overflowed() &&
                          parseSyncStateDocument(verifiedDoc, verifiedState) &&
                          verifiedState.version == state.version && verifiedState.deleted == state.deleted;
    if (verify) verify.close();
    if (!verified ||
        !StorageManager::promoteTempFileWithRollback(filesystem, tmpPath.c_str(), metaPath.c_str())) {
        filesystem.remove(tmpPath);
        return false;
    }
    return true;
}

bool copyProfileFileAs(fs::FS& source, const String& sourcePath, fs::FS& target, const String& targetPath,
                       const String& expectedName) {
    const ProfileFileInspection sourceInspection = inspectProfileFile(source, sourcePath, expectedName);
    if (!sourceInspection.valid) return false;

    File in = source.open(sourcePath, FILE_READ);
    if (!in || in.size() == 0 || in.size() > V1_PROFILE_FILE_MAX_BYTES) {
        if (in) in.close();
        return false;
    }
    String tmpPath;
    if (!appendPathSuffixChecked(targetPath, ".tmpsync", tmpPath)) {
        in.close();
        return false;
    }
    File out = target.open(tmpPath, FILE_WRITE);
    if (!out) {
        in.close();
        return false;
    }
    bool ok = true;
    uint8_t buffer[256];
    while (in.available()) {
        const size_t count = in.read(buffer, sizeof(buffer));
        if (count == 0 || out.write(buffer, count) != count) {
            ok = false;
            break;
        }
    }
    out.flush();
    out.close();
    in.close();
    const ProfileFileInspection copiedInspection = inspectProfileFile(target, tmpPath, expectedName);
    if (!ok || !copiedInspection.valid || copiedInspection.contentCrc != sourceInspection.contentCrc ||
        !StorageManager::promoteTempFileWithRollback(target, tmpPath.c_str(), targetPath.c_str())) {
        target.remove(tmpPath);
        return false;
    }
    return true;
}

bool copyProfileFile(fs::FS& source, fs::FS& target, const String& profilePath, const String& expectedName) {
    return copyProfileFileAs(source, profilePath, target, profilePath, expectedName);
}

bool restoreSyncState(fs::FS& filesystem, const String& profilePath, const ProfileSyncState& state) {
    String metaPath;
    String tmpPath;
    if (!syncMetaPath(profilePath, metaPath) ||
        !appendPathSuffixChecked(metaPath, ".tmp", tmpPath)) return false;
    filesystem.remove(tmpPath);
    if (state.status == ProfileSyncState::Status::Corrupt ||
        state.status == ProfileSyncState::Status::Unavailable) {
        return false;
    }
    if (state.status == ProfileSyncState::Status::Absent ||
        state.status == ProfileSyncState::Status::LegacyImplicit) {
        return !filesystem.exists(metaPath) || filesystem.remove(metaPath);
    }
    return writeSyncState(filesystem, profilePath, state);
}

bool addUniqueName(std::array<String, V1_PROFILE_CATALOG_MAX_COUNT>& names,
                   size_t& nameCount, const String& candidate, bool& overLimit) {
    for (size_t index = 0; index < nameCount; ++index) {
        if (names[index] == candidate) return true;
    }
    if (nameCount >= names.size()) {
        overLimit = true;
        return false;
    }
    names[nameCount] = candidate;
    if (names[nameCount].length() != candidate.length() || names[nameCount] != candidate) return false;
    ++nameCount;
    return true;
}

bool checkedStringFromBytes(const char* bytes, size_t length, String& output) {
    if (!bytes || length > 256u) return false;
    char terminated[257];
    if (length != 0) std::memcpy(terminated, bytes, length);
    terminated[length] = '\0';
    String parsed(terminated);
    if (parsed.length() != length ||
        (length != 0 && std::memcmp(parsed.c_str(), bytes, length) != 0)) return false;
    output = std::move(parsed);
    return output.length() == length &&
           (length == 0 || std::memcmp(output.c_str(), bytes, length) == 0);
}

bool buildProfilePathChecked(const String& directory, const String& name, String& output) {
    const size_t expected = directory.length() + 1u + name.length() + sizeof(".json") - 1u;
    String candidate;
    candidate.reserve(expected);
    candidate += directory;
    candidate += '/';
    candidate += name;
    candidate += ".json";
    if (candidate.length() != expected ||
        std::memcmp(candidate.c_str(), directory.c_str(), directory.length()) != 0 ||
        candidate[directory.length()] != '/' ||
        std::memcmp(candidate.c_str() + directory.length() + 1u, name.c_str(), name.length()) != 0 ||
        std::memcmp(candidate.c_str() + expected - (sizeof(".json") - 1u), ".json",
                    sizeof(".json") - 1u) != 0) return false;
    output = std::move(candidate);
    return output.length() == expected;
}

enum class DirectoryProfileNameStatus : uint8_t { Profile = 0, Ignore, Unavailable };

DirectoryProfileNameStatus directoryProfileName(const char* rawPath, const char* suffix,
                                                String& canonical) {
    if (!rawPath || !suffix) return DirectoryProfileNameStatus::Unavailable;
    const size_t rawLength = strnlen(rawPath, 257u);
    if (rawLength == 257u) return DirectoryProfileNameStatus::Unavailable;
    const char* basename = std::strrchr(rawPath, '/');
    basename = basename ? basename + 1 : rawPath;
    const size_t basenameLength = rawLength - static_cast<size_t>(basename - rawPath);
    const size_t suffixLength = std::strlen(suffix);
    if (basenameLength <= suffixLength ||
        std::memcmp(basename + basenameLength - suffixLength, suffix, suffixLength) != 0) {
        return DirectoryProfileNameStatus::Ignore;
    }
    const size_t nameLength = basenameLength - suffixLength;
    if (nameLength == 0 || nameLength > MAX_PROFILE_NAME_LEN ||
        !validV1Utf8(basename, nameLength)) return DirectoryProfileNameStatus::Ignore;
    String name;
    if (!checkedStringFromBytes(basename, nameLength, name)) {
        return DirectoryProfileNameStatus::Unavailable;
    }
    String parsed;
    const ProfileNameStatus nameStatus = canonicalizeProfileName(name, parsed);
    if (nameStatus == ProfileNameStatus::Valid && parsed == name) {
        canonical = std::move(parsed);
        return canonical.length() == nameLength ? DirectoryProfileNameStatus::Profile
                                                : DirectoryProfileNameStatus::Unavailable;
    }
    // A canonical output shorter than an input without trimmable boundary
    // whitespace is an allocation failure, not an invalid filename.
    if (nameLength != 0 && name[0] != ' ' && name[nameLength - 1] != ' ' && parsed.length() == 0) {
        return DirectoryProfileNameStatus::Unavailable;
    }
    return DirectoryProfileNameStatus::Ignore;
}

enum class ProfileCatalogScanStatus : uint8_t { Success = 0, IoError, Corrupt, Collision };

struct ProfileCatalogSaveScan {
    ProfileCatalogScanStatus status = ProfileCatalogScanStatus::Success;
    bool targetExists = false;
};

// The save transaction needs both a parsed detector candidate and a complete
// post-promotion profile for readback verification. They remain bounded by the
// profile schema, but retaining both on loopTask stacks the full objects below
// boot restore state. A single nothrow scratch allocation keeps the exact same
// validation and comparison boundary without publishing partial data.
struct ProfileSaveScratch {
    V1DetectorConfiguration validatedDetector;
    V1Profile verifiedProfile;
};

// Transaction rollback must be able to restore one journaled member into a
// grandfathered over-limit catalog. Scan one directory entry at a time so the
// rollback path does not need to retain an attacker-sized catalog vector.
ProfileCatalogSaveScan scanCatalogForGrandfatheredRestore(fs::FS& filesystem,
                                                           const String& profileDirectory,
                                                           const String& targetName) {
    ProfileCatalogSaveScan result;
    File dir = filesystem.open(profileDirectory);
    if (!dir || !dir.isDirectory()) {
        if (dir) dir.close();
        result.status = ProfileCatalogScanStatus::IoError;
        return result;
    }

    File entry;
    while ((entry = dir.openNextFile())) {
        String name;
        const DirectoryProfileNameStatus nameStatus = directoryProfileName(entry.name(), ".json", name);
        entry.close();
        if (nameStatus == DirectoryProfileNameStatus::Unavailable) {
            result.status = ProfileCatalogScanStatus::IoError;
            break;
        }
        if (nameStatus != DirectoryProfileNameStatus::Profile) continue;

        String path;
        if (!buildProfilePathChecked(profileDirectory, name, path)) {
            result.status = ProfileCatalogScanStatus::IoError;
            break;
        }
        const ProfileSyncState state = readSyncState(filesystem, path);
        if (state.status == ProfileSyncState::Status::Unavailable) {
            result.status = ProfileCatalogScanStatus::IoError;
            break;
        }
        if (state.status == ProfileSyncState::Status::Corrupt) {
            result.status = ProfileCatalogScanStatus::Corrupt;
            break;
        }
        if (state.deleted) continue;

        if (name == targetName) {
            result.targetExists = true;
        } else if (profileCanonicalNamesCollide(name, targetName)) {
            result.status = ProfileCatalogScanStatus::Collision;
            break;
        }
    }
    dir.close();
    return result;
}

} // namespace

namespace {

bool detectorConfigurationCrc(const V1DetectorConfiguration& config, uint32_t& out, uint8_t schemaVersion) {
    PsramJson::Document doc;
    if (schemaVersion == V1_PROFILE_PREVIOUS_SCHEMA_VERSION) {
        appendV1DetectorConfigurationV2(doc.to<JsonObject>(), config);
    } else {
        appendV1DetectorConfiguration(doc.to<JsonObject>(), config);
    }
    if (doc.overflowed()) return false;
    out = BackupPayloadBuilder::computeBackupCrc32(doc);
    return !doc.overflowed();
}

} // namespace

V1ProfileManager::V1ProfileManager()
    : fs_(nullptr), secondaryFs_(nullptr), storage_(nullptr), usingSd_(false), ready_(false),
      profileDir_("/v1profiles"), currentValid_(false) {}

#ifdef UNIT_TEST
void V1ProfileManager::utFailAllocation(V1ProfileAllocationFailurePoint point, size_t occurrence,
                                        bool repeat) {
    allocationFailurePoint_ = point;
    allocationFailureCountdown_ = occurrence;
    repeatAllocationFailure_ = repeat;
}

void V1ProfileManager::maybeFailAllocationForTest(V1ProfileAllocationFailurePoint point) const {
    if (allocationFailurePoint_ != point || allocationFailureCountdown_ == 0) return;
    if (--allocationFailureCountdown_ != 0) return;
    if (!repeatAllocationFailure_) allocationFailurePoint_ = V1ProfileAllocationFailurePoint::None;
    throw std::bad_alloc();
}
#endif

void V1ProfileManager::bumpCatalogRevision() {
    if (catalogRevisionCounter_ == UINT32_MAX) {
        catalogRevisionCounter_ = 1;
        return;
    }
    catalogRevisionCounter_++;
}

bool V1ProfileManager::recoverInterruptedSavesUnlocked() {
    // Recovery directories can be externally populated. Process one owned
    // transaction artifact per scan so RAM use never scales with directory
    // cardinality, and re-open after each mutation so iterator invalidation
    // cannot skip an entry.
    const auto findOwnedArtifact = [&](const char* suffix, bool orphanOnly, String& livePath,
                                       String& artifactPath) -> DirectoryProfileNameStatus {
        File dir = fs_->open(profileDir_);
        if (!dir || !dir.isDirectory()) {
            if (dir) dir.close();
            return DirectoryProfileNameStatus::Unavailable;
        }
        File entry;
        while ((entry = dir.openNextFile())) {
            String canonical;
            const DirectoryProfileNameStatus status = directoryProfileName(entry.name(), suffix, canonical);
            entry.close();
            if (status == DirectoryProfileNameStatus::Unavailable) {
                dir.close();
                return status;
            }
            if (status != DirectoryProfileNameStatus::Profile) continue;
            if (!buildProfilePathChecked(profileDir_, canonical, livePath)) {
                dir.close();
                return DirectoryProfileNameStatus::Unavailable;
            }
            const bool metadata = std::strstr(suffix, ".meta.") != nullptr;
            if (metadata) {
                String metaPath;
                if (!syncMetaPath(livePath, metaPath)) {
                    dir.close();
                    return DirectoryProfileNameStatus::Unavailable;
                }
                livePath = std::move(metaPath);
            }
            const char* transactionSuffix = std::strstr(suffix, ".tmp") ? ".tmp" : ".bak";
            if (!appendPathSuffixChecked(livePath, transactionSuffix, artifactPath)) {
                dir.close();
                return DirectoryProfileNameStatus::Unavailable;
            }
            if (orphanOnly && fs_->exists(livePath)) continue;
            dir.close();
            return DirectoryProfileNameStatus::Profile;
        }
        dir.close();
        return DirectoryProfileNameStatus::Ignore;
    };

    for (const char* suffix : {".json.tmp", ".json.meta.tmp"}) {
        while (true) {
            String livePath;
            String tmpPath;
            const DirectoryProfileNameStatus status = findOwnedArtifact(suffix, false, livePath, tmpPath);
            if (status == DirectoryProfileNameStatus::Unavailable) return false;
            if (status == DirectoryProfileNameStatus::Ignore) break;
            Serial.println("[V1Profiles] Removing incomplete temp file");
            if (!fs_->remove(tmpPath)) return false;
        }
    }

    for (const char* suffix : {".json.bak", ".json.meta.bak"}) {
        while (true) {
            String livePath;
            String bakPath;
            const DirectoryProfileNameStatus status = findOwnedArtifact(suffix, true, livePath, bakPath);
            if (status == DirectoryProfileNameStatus::Unavailable) return false;
            if (status == DirectoryProfileNameStatus::Ignore) break;
            // Never replace a live file. A retained backup beside a live file
            // is not an interrupted promotion and stays available for manual
            // diagnosis/cleanup.
            if (fs_->exists(livePath)) return false;
            Serial.println("[V1Profiles] RECOVERY: Main file missing, restoring from backup");
            if (!fs_->rename(bakPath, livePath)) return false;
        }
    }
    return true;
}

bool V1ProfileManager::begin(StorageManager& storage) {
    storage_ = &storage;
    usingSd_ = storage.isSDCard();
    return begin(storage.getFilesystem(), storage.getLittleFS());
}

bool V1ProfileManager::begin(fs::FS* filesystem, fs::FS* importFilesystem) {
    ready_ = false;
    lastError_ = "";
    if (!filesystem) {
        Serial.println("[V1Profiles] No filesystem provided");
        ;
        return false;
    }

    fs_ = filesystem;
    secondaryFs_ = importFilesystem != filesystem ? importFilesystem : nullptr;

    ProfileStorageGuard guard(*this, storage_, usingSd_, 500);
    if (!guard.acquired()) {
        lastError_ = "Profile storage busy during initialization";
        Serial.println("[V1Profiles] BUSY: initialization could not acquire SD mutex");
        return false;
    }

    const auto ensureProfileDirectory = [&](fs::FS* filesystem, const char* role) {
        if (!filesystem) return true;
        if (!filesystem->exists(profileDir_) && !filesystem->mkdir(profileDir_)) {
            Serial.printf("[V1Profiles] Failed to create profiles directory on %s storage\n", role);
            return false;
        }
        File directory = filesystem->open(profileDir_);
        const bool ready = directory && directory.isDirectory();
        if (directory) directory.close();
        if (!ready) {
            Serial.printf("[V1Profiles] Profiles path is not a directory on %s storage\n", role);
        }
        return ready;
    };

    // Every configured mirror can become authoritative after an SD-card change.
    // Establish both parent directories before reconciliation or any mirrored
    // tombstone/write so an empty secondary store cannot strand recovery.
    if (!ensureProfileDirectory(fs_, "primary") ||
        !ensureProfileDirectory(secondaryFs_, "secondary")) {
        lastError_ = "Profile storage directory unavailable";
        return false;
    }

    if (importFilesystem && importFilesystem != fs_) {
        // Keep the readable primary catalog available for maintenance if its
        // mirror cannot be reconciled; the reconciler records that degraded
        // state in lastError_ as well as the serial log.
        size_t migrated = reconcileProfilesFrom(importFilesystem);
        if (migrated > 0) {
            Serial.printf("[V1Profiles] Migrated %u profile(s) from secondary filesystem\n",
                          static_cast<unsigned>(migrated));
        }
    }

    // Run startup integrity check - recover any interrupted saves
    if (!recoverInterruptedSavesUnlocked()) {
        lastError_ = "Profile save recovery unavailable";
        return false;
    }

    ready_ = true;
    Serial.println("[V1Profiles] Initialized");
    return true;
}

size_t V1ProfileManager::reconcileProfilesFrom(fs::FS* sourceFs) {
    if (!sourceFs || !fs_ || sourceFs == fs_) {
        return 0;
    }
    if (!sourceFs->exists(profileDir_)) {
        return 0;
    }

    if (!fs_->exists(profileDir_)) fs_->mkdir(profileDir_);

    // Deleted names remain as durable tombstones and are not part of the live
    // catalog limit. Scan all history before changing either store, then visit
    // it in bounded pages so retained RAM does not grow with past deletions.
    auto scanNames = [&](fs::FS& filesystem, const auto& visit) {
        File dir = filesystem.open(profileDir_);
        if (!dir || !dir.isDirectory()) {
            if (dir) dir.close();
            return false;
        }
        File entry;
        while ((entry = dir.openNextFile())) {
            const char* rawPath = entry.name();
            String canonical;
            DirectoryProfileNameStatus status = DirectoryProfileNameStatus::Ignore;
            const size_t rawLength = rawPath ? strnlen(rawPath, 257u) : 257u;
            if (rawLength <= 256u && rawLength >= sizeof(".json.meta") - 1u &&
                std::memcmp(rawPath + rawLength - (sizeof(".json.meta") - 1u), ".json.meta",
                            sizeof(".json.meta") - 1u) == 0) {
                status = directoryProfileName(rawPath, ".json.meta", canonical);
            } else {
                status = directoryProfileName(rawPath, ".json", canonical);
            }
            entry.close();
            if (status == DirectoryProfileNameStatus::Unavailable ||
                (status == DirectoryProfileNameStatus::Profile && !visit(canonical))) {
                dir.close();
                return false;
            }
        }
        dir.close();
        return true;
    };

    // Inspect the complete candidate set before mutating either store. A
    // transient PSRAM or filesystem-read failure must never be reclassified as
    // corruption and cause an older readable copy to overwrite a newer but
    // temporarily unavailable profile or tombstone.
    struct ReconcileCandidate {
        String name;
        String path;
        ProfileSyncState sourceState;
        ProfileSyncState targetState;
        ProfileFileInspection sourceFile;
        ProfileFileInspection targetFile;
    };
    auto inspect = [&](const String& name, ReconcileCandidate& candidate) {
        if (!checkedStringFromBytes(name.c_str(), name.length(), candidate.name) ||
            !buildProfilePathChecked(profileDir_, name, candidate.path)) return false;
        candidate.sourceState = readSyncState(*sourceFs, candidate.path);
        candidate.targetState = readSyncState(*fs_, candidate.path);
        candidate.sourceFile = inspectProfileFile(*sourceFs, candidate.path, name);
        candidate.targetFile = inspectProfileFile(*fs_, candidate.path, name);
        return candidate.sourceState.status != ProfileSyncState::Status::Unavailable &&
               candidate.targetState.status != ProfileSyncState::Status::Unavailable &&
               !candidate.sourceFile.unavailable && !candidate.targetFile.unavailable;
    };

    {
        std::array<String, V1_PROFILE_CATALOG_MAX_COUNT> liveNames;
        size_t liveCount = 0;
        bool liveOverLimit = false;
        ReconcileCandidate inspected;
        auto preflight = [&](const String& name) {
            if (!inspect(name, inspected)) return false;
            const bool sourceUsable = inspected.sourceState.status != ProfileSyncState::Status::Corrupt &&
                                      inspected.sourceState.version > 0 &&
                                      (inspected.sourceState.deleted || inspected.sourceFile.valid);
            const bool targetUsable = inspected.targetState.status != ProfileSyncState::Status::Corrupt &&
                                      inspected.targetState.version > 0 &&
                                      (inspected.targetState.deleted || inspected.targetFile.valid);
            if (!sourceUsable && !targetUsable) return true;
            bool sourceWins = sourceUsable && !targetUsable;
            if (sourceUsable && targetUsable) {
                sourceWins = inspected.sourceState.version > inspected.targetState.version ||
                             (inspected.sourceState.version == inspected.targetState.version &&
                              (inspected.sourceState.deleted != inspected.targetState.deleted
                                   ? inspected.sourceState.deleted
                                   : !inspected.sourceState.deleted &&
                                         inspected.sourceFile.contentCrc != inspected.targetFile.contentCrc));
            }
            if (sourceWins ? inspected.sourceState.deleted : inspected.targetState.deleted) return true;
            return addUniqueName(liveNames, liveCount, name, liveOverLimit);
        };
        if (!scanNames(*sourceFs, preflight) || !scanNames(*fs_, preflight)) {
            lastError_ = liveOverLimit ? "Profile mirror live catalog exceeds supported bound"
                                       : "Profile mirror inspection unavailable";
            Serial.println(liveOverLimit
                               ? "[V1Profiles] RECONCILE live catalog exceeds supported bound; no changes made"
                               : "[V1Profiles] RECONCILE catalog inspection unavailable; no changes made");
            return 0;
        }
    }

    size_t reconciled = 0;
    auto reconcileCandidate = [&](const ReconcileCandidate& candidate) {
        const String& name = candidate.name;
        const String& path = candidate.path;
        const ProfileSyncState& sourceState = candidate.sourceState;
        const ProfileSyncState& targetState = candidate.targetState;
        const ProfileFileInspection& sourceFile = candidate.sourceFile;
        const ProfileFileInspection& targetFile = candidate.targetFile;
        const bool sourceCorrupt = sourceState.status == ProfileSyncState::Status::Corrupt;
        const bool targetCorrupt = targetState.status == ProfileSyncState::Status::Corrupt;
        const bool sourceUsable = !sourceCorrupt && sourceState.version > 0 &&
                                  (sourceState.deleted || sourceFile.valid);
        const bool targetUsable = !targetCorrupt && targetState.version > 0 &&
                                  (targetState.deleted || targetFile.valid);

        if (!sourceUsable && !targetUsable) {
            if (sourceState.version > 0 || targetState.version > 0 || sourceCorrupt || targetCorrupt) {
                Serial.printf("[V1Profiles] RECONCILE no valid copy name='%s' path='%s'\n", name.c_str(),
                              path.c_str());
            }
            return;
        }

        bool sourceWins = sourceUsable && !targetUsable;
        bool needsNewGeneration = false;
        if (sourceUsable && targetUsable) {
            sourceWins = sourceState.version > targetState.version;
            if (sourceState.version == targetState.version) {
                if (sourceState.deleted != targetState.deleted) {
                    sourceWins = sourceState.deleted; // equal-generation deletion always wins
                } else if (!sourceState.deleted && sourceFile.contentCrc != targetFile.contentCrc) {
                    // The secondary store is the only place edits can be made
                    // while the primary SD store is absent. If both stores
                    // independently reach the same generation, prefer that
                    // offline edit and advance the generation so the conflict
                    // cannot recur on the next boot.
                    sourceWins = true;
                    needsNewGeneration = true;
                }
            }
        }

        fs::FS* winner = sourceWins ? sourceFs : fs_;
        fs::FS* loser = sourceWins ? fs_ : sourceFs;
        ProfileSyncState winningState = sourceWins ? sourceState : targetState;
        ProfileSyncState losingState = sourceWins ? targetState : sourceState;

        // A valid older copy must outrank a corrupt higher-generation live
        // copy. Advance beyond both observed generations before repairing it.
        const bool loserUsable = sourceWins ? targetUsable : sourceUsable;
        if (!loserUsable && losingState.version >= winningState.version) {
            needsNewGeneration = true;
        }
        if (needsNewGeneration) {
            const uint32_t maximumVersion = std::max(sourceState.version, targetState.version);
            if (maximumVersion == std::numeric_limits<uint32_t>::max()) {
                Serial.printf("[V1Profiles] RECONCILE generation exhausted name='%s' path='%s'\n",
                              name.c_str(), path.c_str());
                return;
            }
            winningState.version = maximumVersion + 1u;
        }

        const bool stateDiffers = winningState.version != losingState.version ||
                                  winningState.deleted != losingState.deleted ||
                                  (!winningState.deleted && !loser->exists(path)) ||
                                  (winningState.deleted && loser->exists(path)) ||
                                  (!winningState.deleted &&
                                   (sourceFile.contentCrc != targetFile.contentCrc || !loserUsable));

        // Commit any conflict-resolution generation to the winner first. If
        // power is lost while repairing the mirror, the validated winner
        // remains authoritative on the next boot.
        if (!writeSyncState(*winner, path, winningState)) {
            Serial.printf("[V1Profiles] RECONCILE winner metadata failed name='%s' path='%s'\n", name.c_str(),
                          path.c_str());
            return;
        }

        if (stateDiffers) {
            bool applied = true;
            if (winningState.deleted) {
                if (loser->exists(path)) applied = loser->remove(path);
            } else {
                applied = winner->exists(path) && copyProfileFile(*winner, *loser, path, name);
            }
            if (!applied || !writeSyncState(*loser, path, winningState)) {
                Serial.printf("[V1Profiles] RECONCILE failed name='%s' path='%s'\n", name.c_str(), path.c_str());
                return;
            }
            reconciled++;
        }
        if (winningState.deleted && winner->exists(path)) {
            winner->remove(path);
        }
        // Materialize metadata for legacy winners so later edits/deletions have
        // an explicit ordering basis on both filesystems.
        writeSyncState(*loser, path, winningState);
    };

    std::array<String, V1_PROFILE_CATALOG_MAX_COUNT> names;
    std::array<ReconcileCandidate, V1_PROFILE_CATALOG_MAX_COUNT> candidates;
    String after;
    while (true) {
        size_t candidateCount = 0;
        auto collectPage = [&](const String& name) {
            if (std::strcmp(name.c_str(), after.c_str()) <= 0) return true;
            size_t position = 0;
            while (position < candidateCount &&
                   std::strcmp(names[position].c_str(), name.c_str()) < 0) ++position;
            if (position < candidateCount && names[position] == name) return true;
            if (position == names.size()) return true;
            const size_t last = std::min(candidateCount, names.size() - 1u);
            for (size_t index = last; index > position; --index) {
                names[index] = std::move(names[index - 1u]);
            }
            if (!checkedStringFromBytes(name.c_str(), name.length(), names[position])) return false;
            if (candidateCount < names.size()) ++candidateCount;
            return true;
        };
        if (!scanNames(*sourceFs, collectPage) || !scanNames(*fs_, collectPage)) {
            lastError_ = "Profile mirror page enumeration unavailable";
            Serial.println("[V1Profiles] RECONCILE page enumeration unavailable");
            return reconciled;
        }
        if (candidateCount == 0) break;
        for (size_t index = 0; index < candidateCount; ++index) {
            if (!inspect(names[index], candidates[index])) {
                lastError_ = "Profile mirror page inspection unavailable";
                Serial.printf("[V1Profiles] RECONCILE unavailable name='%s'; no further changes made\n",
                              names[index].c_str());
                return reconciled;
            }
        }
        if (!checkedStringFromBytes(names[candidateCount - 1u].c_str(),
                                    names[candidateCount - 1u].length(), after)) {
            lastError_ = "Profile mirror page cursor unavailable";
            Serial.println("[V1Profiles] RECONCILE page cursor unavailable");
            return reconciled;
        }
        for (size_t candidateIndex = 0; candidateIndex < candidateCount; ++candidateIndex) {
            reconcileCandidate(candidates[candidateIndex]);
        }
    }
    return reconciled;
}

String V1ProfileManager::profilePath(const String& name) const {
    String output;
    buildProfilePathChecked(profileDir_, name, output);
    return output;
}

ProfileListResult V1ProfileManager::listProfilesUnlocked() const {
    ProfileListResult result;
    if (!ready_ || !fs_) {
        result.status = ProfileStorageStatus::IoError;
        result.error = "Profile filesystem not ready";
        return result;
    }

    File dir = fs_->open(profileDir_);
    if (!dir || !dir.isDirectory()) {
        if (dir) dir.close();
        result.status = ProfileStorageStatus::IoError;
        result.error = "Failed to enumerate profile directory";
        return result;
    }

    File entry;
    while ((entry = dir.openNextFile())) {
        const char* rawPath = entry.name();
        String name;
        const DirectoryProfileNameStatus nameStatus = directoryProfileName(rawPath, ".json", name);
        entry.close();
        if (nameStatus == DirectoryProfileNameStatus::Unavailable) {
            dir.close();
            result.status = ProfileStorageStatus::IoError;
            result.error = "Profile catalog filename memory unavailable";
            return result;
        }
        if (nameStatus == DirectoryProfileNameStatus::Profile) {
            String path;
            if (!buildProfilePathChecked(profileDir_, name, path)) {
                dir.close();
                result.status = ProfileStorageStatus::IoError;
                result.error = "Profile path memory unavailable";
                return result;
            }
            const ProfileSyncState syncState = readSyncState(*fs_, path);
            if (syncState.status == ProfileSyncState::Status::Unavailable) {
                dir.close();
                result.status = ProfileStorageStatus::IoError;
                result.error = "Profile reconciliation metadata unavailable";
                return result;
            }
            if (syncState.status == ProfileSyncState::Status::Corrupt) {
                dir.close();
                result.status = ProfileStorageStatus::Corrupt;
                result.error = "Corrupt profile reconciliation metadata";
                return result;
            }
            if (syncState.deleted) {
                continue;
            }
            bool collision = false;
            for (const String& existingName : result.profiles) {
                if (profileCanonicalNamesCollide(existingName, name)) {
                    collision = true;
                    break;
                }
            }
            if (collision) {
                dir.close();
                result.status = ProfileStorageStatus::Corrupt;
                result.error = "Canonical profile-name collision in catalog";
                return result;
            }
            if (result.profiles.size() >= V1_PROFILE_CATALOG_MAX_COUNT) {
                dir.close();
                result.status = ProfileStorageStatus::IoError;
                result.error = V1_PROFILE_CATALOG_LIMIT_ERROR;
                result.profiles.clear();
                return result;
            }
            const size_t nameLength = name.length();
            try {
#ifdef UNIT_TEST
                maybeFailAllocationForTest(V1ProfileAllocationFailurePoint::ListGrowth);
#endif
                result.profiles.push_back(std::move(name));
            } catch (const std::bad_alloc&) {
                dir.close();
                result.status = ProfileStorageStatus::IoError;
                result.error = "Profile catalog memory unavailable";
                result.profiles.clear();
                return result;
            }
            if (result.profiles.back().length() != nameLength) {
                dir.close();
                result.status = ProfileStorageStatus::IoError;
                result.error = "Profile catalog memory unavailable";
                return result;
            }
        }
    }
    dir.close();
    result.status = ProfileStorageStatus::Success;
    result.genuinelyEmpty = result.profiles.empty();
    return result;
}

ProfileListResult V1ProfileManager::listProfilesResult(uint32_t timeoutMs) const {
    ProfileStorageGuard guard(*this, storage_, usingSd_, timeoutMs);
    if (!guard.acquired()) {
        ProfileListResult result;
        result.status = ProfileStorageStatus::Busy;
        result.error = "Profile storage busy";
        return result;
    }
    return listProfilesUnlocked();
}

ProfilePageResult V1ProfileManager::listProfilesPageResult(const String& after, size_t limit,
                                                           uint32_t timeoutMs) const {
    ProfilePageResult result;
    if (limit == 0 || limit > V1_PROFILE_CATALOG_MAX_COUNT) {
        result.status = ProfileStorageStatus::InvalidName;
        result.error = "Profile page limit must be from 1 to 10";
        return result;
    }
    if (after.length() > 0) {
        String canonicalAfter;
        if (canonicalizeProfileName(after, canonicalAfter) != ProfileNameStatus::Valid ||
            canonicalAfter != after) {
            result.status = ProfileStorageStatus::InvalidName;
            result.error = "Invalid profile page cursor";
            return result;
        }
    }

    ProfileStorageGuard guard(*this, storage_, usingSd_, timeoutMs);
    if (!guard.acquired()) {
        result.status = ProfileStorageStatus::Busy;
        result.error = "Profile storage busy";
        return result;
    }
    if (!ready_ || !fs_) {
        result.status = ProfileStorageStatus::IoError;
        result.error = "Profile filesystem not ready";
        return result;
    }

    const auto canonicalEntryName = [](const char* rawPath, String& canonical) {
        return directoryProfileName(rawPath, ".json", canonical);
    };
    const auto activeEntry = [&](const String& canonical, ProfileStorageStatus& status) {
        String path;
        if (!buildProfilePathChecked(profileDir_, canonical, path)) {
            status = ProfileStorageStatus::IoError;
            return false;
        }
        const ProfileSyncState syncState = readSyncState(*fs_, path);
        if (syncState.status == ProfileSyncState::Status::Unavailable) {
            status = ProfileStorageStatus::IoError;
            return false;
        }
        if (syncState.status == ProfileSyncState::Status::Corrupt) {
            status = ProfileStorageStatus::Corrupt;
            return false;
        }
        status = ProfileStorageStatus::Success;
        return !syncState.deleted;
    };

    File dir = fs_->open(profileDir_);
    if (!dir || !dir.isDirectory()) {
        if (dir) dir.close();
        result.status = ProfileStorageStatus::IoError;
        result.error = "Failed to enumerate profile directory";
        return result;
    }

    size_t afterCount = 0;
    File entry;
    while ((entry = dir.openNextFile())) {
        const char* rawPath = entry.name();
        String canonical;
        const DirectoryProfileNameStatus nameStatus = canonicalEntryName(rawPath, canonical);
        entry.close();
        if (nameStatus == DirectoryProfileNameStatus::Unavailable) {
            dir.close();
            result.status = ProfileStorageStatus::IoError;
            result.error = "Profile catalog filename memory unavailable";
            return result;
        }
        if (nameStatus != DirectoryProfileNameStatus::Profile) continue;
        ProfileStorageStatus entryStatus = ProfileStorageStatus::Success;
        if (!activeEntry(canonical, entryStatus)) {
            if (entryStatus != ProfileStorageStatus::Success) {
                dir.close();
                result.status = entryStatus;
                result.error = entryStatus == ProfileStorageStatus::Corrupt
                                   ? "Corrupt profile reconciliation metadata"
                                   : "Profile reconciliation metadata unavailable";
                return result;
            }
            continue;
        }
        ++result.total;

        // Keep legacy over-limit catalogs fully enumerable without retaining an
        // unbounded name/key vector. This exact O(n^2) collision pass is used
        // only by the maintenance listing path; supported catalogs are ten.
        File peers = fs_->open(profileDir_);
        if (!peers || !peers.isDirectory()) {
            if (peers) peers.close();
            dir.close();
            result.status = ProfileStorageStatus::IoError;
            result.error = "Failed to validate profile catalog";
            return result;
        }
        File peer;
        while ((peer = peers.openNextFile())) {
            const char* peerPath = peer.name();
            String peerName;
            const DirectoryProfileNameStatus peerNameStatus = canonicalEntryName(peerPath, peerName);
            peer.close();
            if (peerNameStatus == DirectoryProfileNameStatus::Unavailable) {
                peers.close();
                dir.close();
                result.status = ProfileStorageStatus::IoError;
                result.error = "Profile catalog filename memory unavailable";
                return result;
            }
            if (peerNameStatus != DirectoryProfileNameStatus::Profile ||
                std::strcmp(peerName.c_str(), canonical.c_str()) <= 0)
                continue;
            ProfileStorageStatus peerStatus = ProfileStorageStatus::Success;
            if (!activeEntry(peerName, peerStatus)) {
                if (peerStatus != ProfileStorageStatus::Success) {
                    peers.close();
                    dir.close();
                    result.status = peerStatus;
                    result.error = "Profile catalog unavailable";
                    return result;
                }
                continue;
            }
            if (profileCanonicalNamesCollide(peerName, canonical)) {
                peers.close();
                dir.close();
                result.status = ProfileStorageStatus::Corrupt;
                result.error = "Canonical profile-name collision in catalog";
                return result;
            }
        }
        peers.close();

        if (after.length() > 0 && std::strcmp(canonical.c_str(), after.c_str()) <= 0) continue;
        ++afterCount;
        try {
#ifdef UNIT_TEST
            maybeFailAllocationForTest(V1ProfileAllocationFailurePoint::PageGrowth);
#endif
            result.profiles.push_back(std::move(canonical));
        } catch (const std::bad_alloc&) {
            dir.close();
            result.status = ProfileStorageStatus::IoError;
            result.error = "Profile page memory unavailable";
            result.profiles.clear();
            return result;
        }
        std::sort(result.profiles.begin(), result.profiles.end(), [](const String& lhs, const String& rhs) {
            return std::strcmp(lhs.c_str(), rhs.c_str()) < 0;
        });
        if (result.profiles.size() > limit) result.profiles.pop_back();
    }
    dir.close();
    result.hasMore = afterCount > result.profiles.size();
    if (!result.profiles.empty()) {
        result.nextCursor = result.profiles.back();
        if (result.nextCursor.length() != result.profiles.back().length()) {
            result.status = ProfileStorageStatus::IoError;
            result.error = "Profile page cursor memory unavailable";
            result.profiles.clear();
            return result;
        }
    }
    result.status = ProfileStorageStatus::Success;
    return result;
}

std::vector<String> V1ProfileManager::listProfiles() const {
    ProfileListResult result = listProfilesResult();
    return result.success() ? std::move(result.profiles) : std::vector<String>{};
}

ProfileOperationResult V1ProfileManager::loadProfileUnlocked(const String& name, V1Profile& profile,
                                                              bool allowTransactionRecovery,
                                                              bool verifyCandidateOwnedBySave) const {
    if (!ready_ || !fs_) {
        return profileResult(ProfileStorageStatus::IoError, "Profile filesystem not ready");
    }

    String path;
    String bakPath;
    if (!buildProfilePathChecked(profileDir_, name, path) ||
        !appendPathSuffixChecked(path, ".bak", bakPath)) {
        return profileResult(ProfileStorageStatus::IoError, "Profile path memory unavailable");
    }

    // A committed tombstone is authoritative even if stale bytes remain after
    // an interrupted delete. This prevents later enumeration or reconciliation
    // from resurrecting a profile whose deletion was already recorded.
    const ProfileSyncState syncState = readSyncState(*fs_, path);
    if (syncState.status == ProfileSyncState::Status::Unavailable) {
        return profileResult(ProfileStorageStatus::IoError, "Profile reconciliation metadata unavailable");
    }
    if (syncState.status == ProfileSyncState::Status::Corrupt) {
        Serial.printf("[V1Profiles] CORRUPT name='%s' path='%s' metadata=true\n", name.c_str(), path.c_str());
        return profileResult(ProfileStorageStatus::Corrupt, "Corrupt profile reconciliation metadata");
    }
    if (!verifyCandidateOwnedBySave && syncState.deleted) {
        Serial.printf("[V1Profiles] NOT_FOUND name='%s' path='%s' tombstoned=true\n", name.c_str(), path.c_str());
        return profileResult(ProfileStorageStatus::NotFound, "Profile not found");
    }

    const bool mainExists = fs_->exists(path);
    File file = mainExists ? fs_->open(path, FILE_READ) : File();
    if (!file) {
        // Try to recover from backup file
        if (allowTransactionRecovery && fs_->exists(bakPath)) {
            Serial.printf("[V1Profiles] RECOVERY name='%s' path='%s' restoring transaction backup\n", name.c_str(),
                          path.c_str());
            // Rename backup to main file
            if (fs_->rename(bakPath, path)) {
                Serial.printf("[V1Profiles] RECOVERY name='%s' path='%s' restored\n", name.c_str(), path.c_str());
                file = fs_->open(path, FILE_READ);
            }
        }

        if (!file) {
            const ProfileStorageStatus status = mainExists ? ProfileStorageStatus::IoError : ProfileStorageStatus::NotFound;
            lastError_ = status == ProfileStorageStatus::NotFound ? "Profile not found" : "Profile open failed";
            Serial.printf("[V1Profiles] %s name='%s' path='%s'\n",
                          status == ProfileStorageStatus::NotFound ? "NOT_FOUND" : "IO_ERROR", name.c_str(),
                          path.c_str());
            return profileResult(status, lastError_);
        }
    }

    // Hard cap JSON size to avoid excessive allocation on small devices
    if (file.size() > V1_PROFILE_FILE_MAX_BYTES) {
        Serial.printf("[V1Profiles] Profile too large (%u bytes), aborting\n", (unsigned)file.size());
        file.close();
        return profileResult(ProfileStorageStatus::Corrupt, "Profile file exceeds size limit");
    }

    // Read file content for CRC validation with RAII-managed storage
    // so all early returns remain leak-safe.
    const size_t fileSize = file.size();
    PsramJson::Buffer fileContent(fileSize);
    if (!fileContent) {
        file.close();
        return profileResult(ProfileStorageStatus::IoError, "Profile parse memory unavailable");
    }
    if (fileSize > 0) {
        const size_t bytesRead = file.read(fileContent.data(), fileSize);
        if (bytesRead != fileSize) {
            lastError_ = "Failed to read complete profile file";
            Serial.printf("[V1Profiles] %s (%u/%u bytes)\n", lastError_.c_str(), static_cast<unsigned>(bytesRead),
                          static_cast<unsigned>(fileSize));
            file.close();
            return profileResult(ProfileStorageStatus::IoError, lastError_);
        }
    }
    file.close();

    const ExactJsonInput::Status exactInput = ExactJsonInput::validate(fileContent.data(), fileSize);
    if (exactInput == ExactJsonInput::Status::MemoryUnavailable) {
        lastError_ = "Profile exact-input memory unavailable";
        return profileResult(ProfileStorageStatus::IoError, lastError_);
    }
    if (exactInput != ExactJsonInput::Status::Ok) {
        lastError_ = "Profile JSON is not one exact unique-key UTF-8 document";
        return profileResult(ProfileStorageStatus::Corrupt, lastError_);
    }
    PsramJson::Document doc;
    DeserializationError err = deserializeJson(doc, fileContent.data(), fileSize);

    if (doc.overflowed() || err == DeserializationError::NoMemory) {
        lastError_ = "Profile parse memory unavailable";
        return profileResult(ProfileStorageStatus::IoError, lastError_);
    }
    if (err) {
        lastError_ = String("JSON parse error: ") + err.c_str();
        Serial.printf("[V1Profiles] %s\n", lastError_.c_str());
        return profileResult(ProfileStorageStatus::Corrupt, lastError_);
    }

    const JsonVariantConst schemaVersion = doc["schemaVersion"];
    const bool hasSchema = !schemaVersion.isUnbound();
    if (hasSchema != !doc["detector"].isUnbound() ||
        hasSchema != !doc["detectorCrc32"].isUnbound()) {
        lastError_ = "Mixed legacy and versioned profile markers";
        return profileResult(ProfileStorageStatus::Corrupt, lastError_);
    }
    if (hasSchema && (!schemaVersion.is<int>() ||
                      (schemaVersion.as<int>() != V1_PROFILE_PREVIOUS_SCHEMA_VERSION &&
                       schemaVersion.as<int>() != V1_PROFILE_SCHEMA_VERSION))) {
        lastError_ = "Unsupported profile schema version";
        return profileResult(ProfileStorageStatus::Corrupt, lastError_);
    }
    String serializedName;
    const ExactV1JsonStringStatus serializedNameStatus =
        exactV1JsonStringChecked(doc["name"], serializedName, MAX_PROFILE_NAME_LEN);
    if (serializedNameStatus == ExactV1JsonStringStatus::Unavailable) {
        lastError_ = "Profile name memory unavailable";
        return profileResult(ProfileStorageStatus::IoError, lastError_);
    }
    if (serializedNameStatus != ExactV1JsonStringStatus::Valid || serializedName != name) {
        lastError_ = "Profile document name does not match canonical filename";
        return profileResult(ProfileStorageStatus::Corrupt, lastError_);
    }
    if (hasSchema) {
        const bool currentSchema = schemaVersion.as<int>() == V1_PROFILE_SCHEMA_VERSION;
        uint32_t profileCrc = 0;
        if (currentSchema != !doc["profileCrc32"].isUnbound()) {
            lastError_ = "Profile integrity marker does not match schema";
            return profileResult(ProfileStorageStatus::Corrupt, lastError_);
        }
        if (currentSchema &&
            (!doc["profileCrc32"].is<uint32_t>() || !profileDocumentCrc(doc, profileCrc) ||
             doc["profileCrc32"].as<uint32_t>() != profileCrc)) {
            lastError_ = doc.overflowed() ? "Profile integrity memory unavailable" : "Profile integrity CRC mismatch";
            return profileResult(doc.overflowed() ? ProfileStorageStatus::IoError : ProfileStorageStatus::Corrupt,
                                 lastError_);
        }
    }

    std::unique_ptr<V1Profile> parsedProfile(new (std::nothrow) V1Profile(name));
    if (!parsedProfile || parsedProfile->name.length() != name.length() ||
        parsedProfile->name != name) {
        return profileResult(ProfileStorageStatus::IoError, "Profile candidate memory unavailable");
    }
    if (hasSchema) {
        const uint8_t parsedSchema = static_cast<uint8_t>(schemaVersion.as<int>());
        uint32_t detectorCrc = 0;
        if (!doc["detector"].is<JsonObjectConst>() ||
            !(parsedSchema == V1_PROFILE_SCHEMA_VERSION
                  ? parseV1DetectorConfiguration(doc["detector"].as<JsonObjectConst>(), parsedProfile->detector)
                  : parseV1DetectorConfigurationV2(doc["detector"].as<JsonObjectConst>(), parsedProfile->detector)) ||
            !doc["detectorCrc32"].is<uint32_t>()) {
            lastError_ = "Invalid detector configuration or CRC";
            return profileResult(ProfileStorageStatus::Corrupt, lastError_);
        }
        if (!detectorConfigurationCrc(parsedProfile->detector, detectorCrc, parsedSchema)) {
            lastError_ = "Detector CRC memory unavailable";
            return profileResult(ProfileStorageStatus::IoError, lastError_);
        }
        if (doc["detectorCrc32"].as<uint32_t>() != detectorCrc) {
            lastError_ = "Invalid detector configuration or CRC";
            return profileResult(ProfileStorageStatus::Corrupt, lastError_);
        }
        if (parsedSchema == V1_PROFILE_PREVIOUS_SCHEMA_VERSION) {
            migrateV1DetectorConfigurationV2InPlace(parsedProfile->detector);
        }
    }

    V1UserSettings parsedLegacySettings;
    bool parsedLegacyDisplayOn = true;
    uint8_t parsedLegacyMainVolume = 0xFF;
    uint8_t parsedLegacyMutedVolume = 0xFF;
    if (!hasSchema &&
        !parseLegacyProfileSettings(doc.as<JsonObjectConst>(), parsedLegacySettings,
                                    parsedLegacyDisplayOn, parsedLegacyMainVolume,
                                    parsedLegacyMutedVolume)) {
        lastError_ = "Invalid legacy profile fields";
        return profileResult(ProfileStorageStatus::Corrupt, lastError_);
    }

    const JsonVariantConst rawBytes = doc["bytes"];
    const bool hasRawBytes = !rawBytes.isUnbound();
    if (hasSchema && (!hasRawBytes || !doc["crc32"].is<uint32_t>())) {
        lastError_ = "Versioned profile is missing settings bytes or CRC";
        return profileResult(ProfileStorageStatus::Corrupt, lastError_);
    }
    uint8_t rawSettingsBytes[V1SettingsJson::kSettingsByteCount];
    if (hasRawBytes) {
        if (!V1SettingsJson::parseRawBytes(rawBytes, rawSettingsBytes)) {
            lastError_ = "Invalid settings bytes";
            Serial.printf("[V1Profiles] %s\n", lastError_.c_str());
            return profileResult(ProfileStorageStatus::Corrupt, lastError_);
        }
        if (hasSchema &&
            !validateVersionedReadableSettings(doc.as<JsonObjectConst>(), rawSettingsBytes,
                                               schemaVersion.as<int>() == V1_PROFILE_SCHEMA_VERSION)) {
            lastError_ = "Versioned readable settings do not match authoritative bytes";
            return profileResult(ProfileStorageStatus::Corrupt, lastError_);
        }
    }

    // Validate CRC32 if present
    if (doc["crc32"].is<uint32_t>()) {
        uint32_t storedCrc = doc["crc32"].as<uint32_t>();

        // Calculate CRC of the 6 settings bytes
        if (hasRawBytes) {
            uint32_t computedCrc = calculateCRC32(rawSettingsBytes, V1SettingsJson::kSettingsByteCount);
            if (storedCrc != computedCrc) {
                lastError_ = "CRC mismatch - profile file corrupted";
                Serial.printf("[V1Profiles] %s (stored: %08lX, computed: %08lX)\n", lastError_.c_str(),
                              static_cast<unsigned long>(storedCrc), static_cast<unsigned long>(computedCrc));
                return profileResult(ProfileStorageStatus::Corrupt, lastError_);
            }
            Serial.println("[V1Profiles] CRC32 validated OK");
        }
    }

    if (hasSchema && !hasRawBytes) {
        lastError_ = "Versioned profile is missing authoritative settings bytes";
        return profileResult(ProfileStorageStatus::Corrupt, lastError_);
    }

    parsedProfile->schemaVersion = hasSchema ? V1_PROFILE_SCHEMA_VERSION : 1;
    const ExactV1JsonStringStatus descriptionStatus = exactV1JsonStringChecked(
        doc["description"], parsedProfile->description, V1_PROFILE_DESCRIPTION_MAX_BYTES);
    if (descriptionStatus == ExactV1JsonStringStatus::Unavailable) {
        lastError_ = "Profile description memory unavailable";
        return profileResult(ProfileStorageStatus::IoError, lastError_);
    }
    if (descriptionStatus != ExactV1JsonStringStatus::Valid) {
        lastError_ = "Profile description is not valid exact UTF-8 or exceeds 4096 bytes";
        return profileResult(ProfileStorageStatus::Corrupt, lastError_);
    }
    if (!hasSchema) {
        parsedProfile->displayOn = parsedLegacyDisplayOn;
        parsedProfile->mainVolume = parsedLegacyMainVolume;
        parsedProfile->mutedVolume = parsedLegacyMutedVolume;
    }

    // Parse settings bytes
    if (!hasSchema) {
        parsedProfile->settings = parsedLegacySettings;
    } else if (hasRawBytes) {
        for (size_t i = 0; i < V1SettingsJson::kSettingsByteCount; i++) {
            parsedProfile->settings.bytes[i] = rawSettingsBytes[i];
        }
    }

    profile = std::move(*parsedProfile);
    Serial.printf("[V1Profiles] LOAD success name='%s' path='%s'\n", name.c_str(), path.c_str());
    return profileResult(ProfileStorageStatus::Success);
}

ProfileOperationResult V1ProfileManager::loadProfileResult(const String& rawName, V1Profile& profile,
                                                           uint32_t timeoutMs) const {
    String canonical;
    const ProfileNameStatus nameStatus = canonicalizeProfileName(rawName, canonical);
    if (nameStatus != ProfileNameStatus::Valid) {
        return profileResult(ProfileStorageStatus::InvalidName, profileNameStatusMessage(nameStatus));
    }
    ProfileStorageGuard guard(*this, storage_, usingSd_, timeoutMs);
    if (!guard.acquired()) {
        lastError_ = "Profile storage busy";
        Serial.printf("[V1Profiles] BUSY name='%s' path='%s'\n", canonical.c_str(), profilePath(canonical).c_str());
        return profileResult(ProfileStorageStatus::Busy, lastError_);
    }
    return loadProfileUnlocked(canonical, profile);
}

bool V1ProfileManager::loadProfile(const String& name, V1Profile& profile) const {
    return loadProfileResult(name, profile).success();
}

ProfileSaveResult V1ProfileManager::saveProfileUnlocked(const V1Profile& profile, const String& canonicalName,
                                                        bool allowGrandfatheredRestore, bool createOnly) {
    if (!ready_ || !fs_) {
        lastError_ = "Filesystem not ready";
        Serial.printf("[V1Profiles] Save failed: %s\n", lastError_.c_str());
        return ProfileSaveResult(ProfileStorageStatus::IoError, lastError_);
    }
    if (!validV1ProfileDescription(profile.description)) {
        lastError_ = "Profile description exceeds 4096 bytes";
        return ProfileSaveResult(ProfileStorageStatus::Corrupt, lastError_);
    }

    bool updatingExisting = false;
    size_t catalogSize = 0;
    if (allowGrandfatheredRestore) {
        const ProfileCatalogSaveScan scan =
            scanCatalogForGrandfatheredRestore(*fs_, profileDir_, canonicalName);
        if (scan.status == ProfileCatalogScanStatus::IoError) {
            return ProfileSaveResult(ProfileStorageStatus::IoError, "Profile catalog unavailable");
        }
        if (scan.status == ProfileCatalogScanStatus::Corrupt) {
            return ProfileSaveResult(ProfileStorageStatus::Corrupt,
                                     "Corrupt profile reconciliation metadata");
        }
        if (scan.status == ProfileCatalogScanStatus::Collision) {
            lastError_ = "Profile name collides with existing canonical name";
            return ProfileSaveResult(ProfileStorageStatus::InvalidName, lastError_);
        }
        updatingExisting = scan.targetExists;
    } else {
        const ProfileListResult catalog = listProfilesUnlocked();
        if (!catalog.success()) {
            return ProfileSaveResult(catalog.status, catalog.error);
        }
        catalogSize = catalog.profiles.size();
        for (const String& existing : catalog.profiles) {
            updatingExisting |= existing == canonicalName;
            if (existing != canonicalName && profileCanonicalNamesCollide(existing, canonicalName)) {
                lastError_ = "Profile name collides with existing canonical name";
                Serial.printf("[V1Profiles] INVALID_NAME name='%s' path='%s' reason=collision\n",
                              canonicalName.c_str(), profilePath(canonicalName).c_str());
                return ProfileSaveResult(ProfileStorageStatus::InvalidName, lastError_);
            }
        }
    }
    if (createOnly && updatingExisting) {
        lastError_ = "Profile already exists";
        return ProfileSaveResult(ProfileStorageStatus::InvalidName, lastError_);
    }
    if (!updatingExisting && catalogSize >= V1_PROFILE_CATALOG_MAX_COUNT &&
        !allowGrandfatheredRestore) {
        lastError_ = V1_PROFILE_CATALOG_LIMIT_ERROR;
        return ProfileSaveResult(ProfileStorageStatus::IoError, lastError_);
    }

    String path;
    String tmpPath;
    String bakPath;
    String secondaryRollbackPath;
    if (!buildProfilePathChecked(profileDir_, canonicalName, path) ||
        !appendPathSuffixChecked(path, ".tmp", tmpPath) ||
        !appendPathSuffixChecked(path, ".bak", bakPath) ||
        (secondaryFs_ && !appendPathSuffixChecked(path, ".syncbak", secondaryRollbackPath))) {
        lastError_ = "Profile path memory unavailable";
        return ProfileSaveResult(ProfileStorageStatus::IoError, lastError_);
    }
    const bool activeFileExisted = fs_->exists(path);
    const ProfileSyncState activeState = readSyncState(*fs_, path);
    const ProfileSyncState secondaryState = secondaryFs_ ? readSyncState(*secondaryFs_, path) : ProfileSyncState{};
    if (activeState.status == ProfileSyncState::Status::Unavailable ||
        secondaryState.status == ProfileSyncState::Status::Unavailable) {
        lastError_ = "Profile reconciliation metadata unavailable";
        return ProfileSaveResult(ProfileStorageStatus::IoError, lastError_);
    }
    if (activeState.status == ProfileSyncState::Status::Corrupt ||
        secondaryState.status == ProfileSyncState::Status::Corrupt) {
        lastError_ = "Corrupt profile reconciliation metadata";
        return ProfileSaveResult(ProfileStorageStatus::Corrupt, lastError_);
    }
    ProfileSyncState committedState;
    const uint32_t maximumVersion = std::max(activeState.version, secondaryState.version);
    if (maximumVersion == std::numeric_limits<uint32_t>::max()) {
        lastError_ = "Profile reconciliation generation exhausted";
        return ProfileSaveResult(ProfileStorageStatus::IoError, lastError_);
    }
    committedState.version = maximumVersion + 1u;
    committedState.deleted = false;

#ifdef UNIT_TEST
    try {
        maybeFailAllocationForTest(V1ProfileAllocationFailurePoint::SaveScratch);
    } catch (const std::bad_alloc&) {
        lastError_ = "Profile save scratch memory unavailable";
        return ProfileSaveResult(ProfileStorageStatus::IoError, lastError_);
    }
#endif
    std::unique_ptr<ProfileSaveScratch> scratch(new (std::nothrow) ProfileSaveScratch());
    if (!scratch) {
        lastError_ = "Profile save scratch memory unavailable";
        return ProfileSaveResult(ProfileStorageStatus::IoError, lastError_);
    }

    // Step 1: Write to temporary file (don't truncate original yet)
    File file = fs_->open(tmpPath, FILE_WRITE);
    if (!file) {
        lastError_ = "Failed to create temp file";
        Serial.printf("[V1Profiles] %s\n", lastError_.c_str());
        return ProfileSaveResult(ProfileStorageStatus::IoError, lastError_);
    }

    PsramJson::Document doc;
    const V1UserSettings& s = profile.settings;

    // Store metadata and the complete detector-owned application policy.
    doc["schemaVersion"] = V1_PROFILE_SCHEMA_VERSION;
    doc["name"] = canonicalName;
    doc["description"] = profile.description;
    JsonObject detector = doc["detector"].to<JsonObject>();
    appendV1DetectorConfiguration(detector, profile.detector);
    if (!parseV1DetectorConfiguration(detector, scratch->validatedDetector) ||
        scratch->validatedDetector != profile.detector) {
        file.close();
        fs_->remove(tmpPath);
        lastError_ = "Invalid detector configuration";
        return ProfileSaveResult(ProfileStorageStatus::Corrupt, lastError_);
    }
    uint32_t detectorCrc = 0;
    if (!detectorConfigurationCrc(profile.detector, detectorCrc)) {
        file.close();
        fs_->remove(tmpPath);
        lastError_ = "Detector CRC memory unavailable";
        return ProfileSaveResult(ProfileStorageStatus::IoError, lastError_);
    }
    doc["detectorCrc32"] = detectorCrc;

    // Store raw bytes for exact restoration
    JsonArray bytes = doc["bytes"].to<JsonArray>();
    for (int i = 0; i < 6; i++) {
        bytes.add(s.bytes[i]);
    }

    // Also store human-readable settings
    doc["xBand"] = s.xBandEnabled();
    doc["kBand"] = s.kBandEnabled();
    doc["kaBand"] = s.kaBandEnabled();
    doc["laser"] = s.laserEnabled();
    doc["kuBand"] = s.kuBandEnabled();
    doc["euro"] = s.euroMode();
    doc["kVerifier"] = s.kVerifier();
    doc["laserRear"] = s.laserRear();
    doc["customFreqs"] = s.customFreqs();
    doc["kaAlwaysPriority"] = s.kaAlwaysPriority();
    doc["fastLaserDetect"] = s.fastLaserDetect();
    doc["kaSensitivity"] = s.kaSensitivity();
    doc["kSensitivity"] = s.kSensitivity();
    doc["xSensitivity"] = s.xSensitivity();
    doc["autoMute"] = s.autoMute();
    doc["muteToMuteVolume"] = s.muteToMuteVolume();
    doc["bogeyLockLoud"] = s.bogeyLockLoud();
    doc["muteXKRear"] = s.muteXKRear();
    doc["startupSequence"] = s.startupSequence();
    doc["restingDisplay"] = s.restingDisplay();
    doc["bsmPlus"] = s.bsmPlus();
    doc["mrct"] = s.mrct();
    doc["driveSafe3D"] = s.driveSafe3D();
    doc["driveSafe3DHD"] = s.driveSafe3DHD();
    doc["redflexHalo"] = s.redflexHalo();
    doc["redflexNK7"] = s.redflexNK7();
    doc["ekin"] = s.ekin();
    doc["photoVerifier"] = s.photoVerifier();
    doc["gatsoRT4"] = s.gatsoRT4();
    doc["photoIntersectionFilter"] = s.photoIntersectionFilter();

    // Calculate and store CRC32 of the settings bytes for integrity checking
    uint32_t crc = calculateCRC32(s.bytes, 6);
    doc["crc32"] = crc;

    uint32_t profileCrc = 0;
    if (!profileDocumentCrc(doc, profileCrc)) {
        file.close();
        fs_->remove(tmpPath);
        lastError_ = "Profile integrity memory unavailable";
        return ProfileSaveResult(ProfileStorageStatus::IoError, lastError_);
    }
    doc["profileCrc32"] = profileCrc;

    const size_t expectedPrettyBytes = measureJsonPretty(doc);
    if (doc.overflowed() || expectedPrettyBytes == 0 || expectedPrettyBytes > V1_PROFILE_FILE_MAX_BYTES) {
        file.close();
        fs_->remove(tmpPath);
        lastError_ = V1_PROFILE_FILE_LIMIT_ERROR;
        return ProfileSaveResult(ProfileStorageStatus::Corrupt, lastError_);
    }
    size_t written = serializeJsonPretty(doc, file);

    // Step 2: Flush to ensure data is written to SD before closing
    file.flush();
    file.close();

    // Step 3: Verify write succeeded and file size matches
    if (written == 0) {
        lastError_ = "Serialization failed - no data written";
        Serial.printf("[V1Profiles] %s\n", lastError_.c_str());
        fs_->remove(tmpPath);
        return ProfileSaveResult(ProfileStorageStatus::IoError, lastError_);
    }
    if (written != expectedPrettyBytes) {
        lastError_ = "Partial write detected: expected " + String(static_cast<unsigned long>(expectedPrettyBytes)) +
                     " bytes, wrote " + String(static_cast<unsigned long>(written));
        Serial.printf("[V1Profiles] %s\n", lastError_.c_str());
        fs_->remove(tmpPath);
        return ProfileSaveResult(ProfileStorageStatus::IoError, lastError_);
    }
    {
        File verify = fs_->open(tmpPath, FILE_READ);
        if (!verify) {
            lastError_ = "Failed to re-open temp file for verification";
            Serial.printf("[V1Profiles] %s\n", lastError_.c_str());
            fs_->remove(tmpPath);
            return ProfileSaveResult(ProfileStorageStatus::IoError, lastError_);
        }
        size_t fileSize = verify.size();
        verify.close();
        if (fileSize != written) {
            lastError_ = "Partial write detected: expected " + String(written) + " bytes, got " + String(fileSize);
            Serial.printf("[V1Profiles] %s\n", lastError_.c_str());
            fs_->remove(tmpPath);
            return ProfileSaveResult(ProfileStorageStatus::IoError, lastError_);
        }
    }

    // Step 4: Create backup of existing file before replacement
    if (activeFileExisted) {
        // Remove old backup if exists
        if (fs_->exists(bakPath)) {
            if (!fs_->remove(bakPath)) {
                lastError_ = "Failed to remove stale profile transaction backup";
                fs_->remove(tmpPath);
                return ProfileSaveResult(ProfileStorageStatus::IoError, lastError_);
            }
        }
        // Rename current to backup (for rollback capability)
        if (!fs_->rename(path, bakPath)) {
            lastError_ = "Failed to create profile transaction backup";
            fs_->remove(tmpPath);
            return ProfileSaveResult(ProfileStorageStatus::IoError, lastError_);
        } else {
            Serial.println("[V1Profiles] Created backup");
        }
    }

    // Step 5: Rename temp to final
    if (!fs_->rename(tmpPath, path)) {
        lastError_ = "Failed to rename temp to final";
        Serial.printf("[V1Profiles] %s\n", lastError_.c_str());

        // Try to restore from backup
        if (fs_->exists(bakPath)) {
            if (fs_->rename(bakPath, path)) {
                Serial.println("[V1Profiles] Restored from backup after failed save");
            }
        }
        fs_->remove(tmpPath);
        return ProfileSaveResult(ProfileStorageStatus::IoError, lastError_);
    }

    // Step 6: prove the promoted final file is readable and its CRC is valid.
    // Do not let ordinary interrupted-transaction recovery consume the backup
    // while validating a just-promoted candidate. The save transaction owns
    // rollback until final-file verification completes.
    // This is the only tombstone bypass. The save transaction has just
    // promoted this exact candidate and still owns rollback; ordinary loads,
    // boot reconciliation, and API reads continue to honor the old tombstone
    // until writeSyncState() commits the new generation below.
    const ProfileOperationResult verifyResult =
        loadProfileUnlocked(canonicalName, scratch->verifiedProfile, false, true);
    if (!verifyResult.success() || scratch->verifiedProfile.schemaVersion != V1_PROFILE_SCHEMA_VERSION ||
        scratch->verifiedProfile.name != canonicalName ||
        scratch->verifiedProfile.description != profile.description ||
        memcmp(scratch->verifiedProfile.settings.bytes, profile.settings.bytes, 6) != 0 ||
        scratch->verifiedProfile.detector != profile.detector) {
        lastError_ = verifyResult.success() ? "Final profile verification mismatch" : verifyResult.error;
        fs_->remove(path);
        if (fs_->exists(bakPath)) {
            fs_->rename(bakPath, path);
        }
        Serial.printf("[V1Profiles] VERIFY_FAILED name='%s' path='%s' reason='%s'\n", canonicalName.c_str(),
                      path.c_str(), lastError_.c_str());
        return ProfileSaveResult(verifyResult.success() ? ProfileStorageStatus::Corrupt : verifyResult.status,
                                 lastError_);
    }

    if (!writeSyncState(*fs_, path, committedState)) {
        lastError_ = "Failed to persist profile reconciliation metadata";
        fs_->remove(path);
        if (fs_->exists(bakPath)) fs_->rename(bakPath, path);
        restoreSyncState(*fs_, path, activeState);
        return ProfileSaveResult(ProfileStorageStatus::IoError, lastError_);
    }

    if (secondaryFs_) {
        const bool secondaryFileExisted = secondaryFs_->exists(path);
        bool secondaryPrepared = true;
        bool secondaryMutationStarted = false;
        if (!secondaryFs_->exists(profileDir_) && !secondaryFs_->mkdir(profileDir_)) {
            secondaryPrepared = false;
        }
        if (secondaryFs_->exists(secondaryRollbackPath) && !secondaryFs_->remove(secondaryRollbackPath)) {
            secondaryPrepared = false;
        }
        if (secondaryPrepared && secondaryFileExisted &&
            !copyProfileFileAs(*secondaryFs_, path, *secondaryFs_, secondaryRollbackPath, canonicalName)) {
            secondaryPrepared = false;
        }

        bool secondaryCommitted = false;
        if (secondaryPrepared) {
            secondaryMutationStarted = true;
            secondaryCommitted = copyProfileFile(*fs_, *secondaryFs_, path, canonicalName) &&
                                 writeSyncState(*secondaryFs_, path, committedState);
        }
        if (!secondaryCommitted) {
            bool rollbackOk = true;

            if (secondaryMutationStarted) {
                if (secondaryFs_->exists(path) && !secondaryFs_->remove(path)) rollbackOk = false;
                if (secondaryFileExisted) {
                    if (!secondaryFs_->exists(secondaryRollbackPath) ||
                        !secondaryFs_->rename(secondaryRollbackPath, path)) {
                        rollbackOk = false;
                    }
                } else if (secondaryFs_->exists(secondaryRollbackPath) &&
                           !secondaryFs_->remove(secondaryRollbackPath)) {
                    rollbackOk = false;
                }
                if (!restoreSyncState(*secondaryFs_, path, secondaryState)) rollbackOk = false;
            }

            if (fs_->exists(path) && !fs_->remove(path)) rollbackOk = false;
            if (activeFileExisted) {
                if (!fs_->exists(bakPath) || !fs_->rename(bakPath, path)) rollbackOk = false;
            } else if (fs_->exists(bakPath) && !fs_->remove(bakPath)) {
                rollbackOk = false;
            }
            if (!restoreSyncState(*fs_, path, activeState)) rollbackOk = false;

            lastError_ = rollbackOk ? "Failed to commit profile to secondary storage"
                                    : "Failed to commit profile and rollback was incomplete";
            Serial.printf("[V1Profiles] SAVE failed name='%s' path='%s' reason='%s'\n", canonicalName.c_str(),
                          path.c_str(), lastError_.c_str());
            return ProfileSaveResult(ProfileStorageStatus::IoError, lastError_);
        }
        if (secondaryFs_->exists(secondaryRollbackPath)) secondaryFs_->remove(secondaryRollbackPath);
    }

    if (fs_->exists(bakPath)) {
        fs_->remove(bakPath);
    }

    Serial.printf("[V1Profiles] SAVE success name='%s' path='%s' bytes=%u crc=%08lX\n", canonicalName.c_str(),
                  path.c_str(), written, static_cast<unsigned long>(crc));
    bumpCatalogRevision();
    return ProfileSaveResult(ProfileStorageStatus::Success);
}

ProfileSaveResult V1ProfileManager::saveProfile(const V1Profile& profile, bool createOnly) {
    String canonical;
    if (std::strlen(profile.name.c_str()) != profile.name.length() ||
        !validV1Utf8(profile.name.c_str(), profile.name.length())) {
        lastError_ = "Profile name is not valid exact UTF-8";
        return ProfileSaveResult(ProfileStorageStatus::InvalidName, lastError_);
    }
    const ProfileNameStatus nameStatus = canonicalizeProfileName(profile.name, canonical);
    if (nameStatus != ProfileNameStatus::Valid) {
        lastError_ = profileNameStatusMessage(nameStatus);
        Serial.printf("[V1Profiles] INVALID_NAME requested='%s' reason='%s'\n", profile.name.c_str(), lastError_.c_str());
        return ProfileSaveResult(ProfileStorageStatus::InvalidName, lastError_);
    }
    ProfileStorageGuard guard(*this, storage_, usingSd_, 250);
    if (!guard.acquired()) {
        lastError_ = "Profile storage busy";
        Serial.printf("[V1Profiles] BUSY name='%s' path='%s'\n", canonical.c_str(), profilePath(canonical).c_str());
        return ProfileSaveResult(ProfileStorageStatus::Busy, lastError_);
    }
    return saveProfileUnlocked(profile, canonical, false, createOnly);
}

ProfileOperationResult V1ProfileManager::deleteProfileUnlocked(const String& name) {
    if (!ready_ || !fs_) {
        return profileResult(ProfileStorageStatus::IoError, "Profile filesystem not ready");
    }

    String path;
    String bakPath;
    if (!buildProfilePathChecked(profileDir_, name, path) ||
        !appendPathSuffixChecked(path, ".bak", bakPath)) {
        return profileResult(ProfileStorageStatus::IoError, "Profile path memory unavailable");
    }
    const ProfileSyncState activeState = readSyncState(*fs_, path);
    const ProfileSyncState secondaryState = secondaryFs_ ? readSyncState(*secondaryFs_, path) : ProfileSyncState{};
    if (activeState.status == ProfileSyncState::Status::Unavailable ||
        secondaryState.status == ProfileSyncState::Status::Unavailable) {
        return profileResult(ProfileStorageStatus::IoError, "Profile reconciliation metadata unavailable");
    }
    if (activeState.status == ProfileSyncState::Status::Corrupt ||
        secondaryState.status == ProfileSyncState::Status::Corrupt) {
        return profileResult(ProfileStorageStatus::Corrupt, "Corrupt profile reconciliation metadata");
    }
    ProfileSyncState tombstone;
    const uint32_t maximumVersion = std::max(activeState.version, secondaryState.version);
    if (maximumVersion == std::numeric_limits<uint32_t>::max()) {
        return profileResult(ProfileStorageStatus::IoError, "Profile reconciliation generation exhausted");
    }
    tombstone.version = maximumVersion + 1u;
    tombstone.deleted = true;
    const bool activeExists = fs_->exists(path);
    const bool activeBakExists = fs_->exists(bakPath);
    const bool secondaryExists = secondaryFs_ && secondaryFs_->exists(path);
    String secondaryBak;
    if (!appendPathSuffixChecked(path, ".bak", secondaryBak)) {
        return profileResult(ProfileStorageStatus::IoError, "Profile path memory unavailable");
    }
    const bool secondaryBakExists = secondaryFs_ && secondaryFs_->exists(secondaryBak);
    const bool removedAny = activeExists || activeBakExists || secondaryExists || secondaryBakExists;

    if (!removedAny) {
        return profileResult(ProfileStorageStatus::NotFound, "Profile not found");
    }

    // Persist deletion intent before removing data. If power is lost after this
    // point, list/load treat any leftover bytes as deleted and reconciliation
    // propagates the higher-generation tombstone.
    if (!writeSyncState(*fs_, path, tombstone)) {
        return profileResult(ProfileStorageStatus::IoError, "Failed to persist profile deletion metadata");
    }

    // Do not acknowledge a delete until every filesystem that can become the
    // active store has the tombstone. Otherwise removing the SD card after a
    // successful response can resurrect the stale LittleFS copy.
    if (secondaryFs_ && !writeSyncState(*secondaryFs_, path, tombstone)) {
        const bool primaryRestored = restoreSyncState(*fs_, path, activeState);
        const bool secondaryRestored = restoreSyncState(*secondaryFs_, path, secondaryState);
        const String error = primaryRestored && secondaryRestored
                                 ? "Failed to commit profile deletion to secondary storage"
                                 : "Failed to commit profile deletion and rollback was incomplete";
        Serial.printf("[V1Profiles] DELETE failed name='%s' path='%s' reason='%s'\n", name.c_str(), path.c_str(),
                      error.c_str());
        return profileResult(ProfileStorageStatus::IoError, error);
    }

    bool ok = true;

    if (activeExists) {
        if (!fs_->remove(path)) {
            ok = false;
        }
    }

    if (activeBakExists) {
        if (!fs_->remove(bakPath)) {
            ok = false;
        }
    }

    if (secondaryFs_) {
        if (secondaryExists) {
            ok = secondaryFs_->remove(path) && ok;
        }
        if (secondaryBakExists) ok = secondaryFs_->remove(secondaryBak) && ok;
    }

    if (removedAny) {
        Serial.printf("[V1Profiles] DELETE success name='%s' path='%s'\n", name.c_str(), path.c_str());
        bumpCatalogRevision();
    }
    if (!ok) {
        Serial.printf("[V1Profiles] WARN: deleted profile bytes remain for later cleanup name='%s' path='%s'\n",
                      name.c_str(), path.c_str());
    }
    return profileResult(ProfileStorageStatus::Success);
}

ProfileOperationResult V1ProfileManager::deleteProfileResult(const String& rawName, uint32_t timeoutMs) {
    String canonical;
    const ProfileNameStatus nameStatus = canonicalizeProfileName(rawName, canonical);
    if (nameStatus != ProfileNameStatus::Valid) {
        return profileResult(ProfileStorageStatus::InvalidName, profileNameStatusMessage(nameStatus));
    }
    ProfileStorageGuard guard(*this, storage_, usingSd_, timeoutMs);
    if (!guard.acquired()) {
        Serial.printf("[V1Profiles] BUSY name='%s' path='%s'\n", canonical.c_str(), profilePath(canonical).c_str());
        return profileResult(ProfileStorageStatus::Busy, "Profile storage busy");
    }
    return deleteProfileUnlocked(canonical);
}

bool V1ProfileManager::deleteProfile(const String& name) {
    return deleteProfileResult(name).success();
}

ProfileSaveResult V1ProfileManager::restoreProfileForTransaction(const V1Profile& profile) {
    String canonical;
    const ProfileNameStatus nameStatus = canonicalizeProfileName(profile.name, canonical);
    if (nameStatus != ProfileNameStatus::Valid || canonical != profile.name) {
        return ProfileSaveResult(ProfileStorageStatus::InvalidName, profileNameStatusMessage(nameStatus));
    }
    ProfileStorageGuard guard(*this, storage_, usingSd_, 250);
    if (!guard.acquired()) return ProfileSaveResult(ProfileStorageStatus::Busy, "Profile storage busy");
    // The caller owns a durable delete journal containing this exact profile.
    // No public create/rename path can request this narrowly scoped bypass.
    return saveProfileUnlocked(profile, canonical, true);
}

ProfileOperationResult V1ProfileManager::snapshotProfiles(std::vector<V1Profile>& profiles, uint32_t timeoutMs,
                                                           bool allowGrandfatheredOverLimit) const {
    profiles.clear();
    ProfileStorageGuard guard(*this, storage_, usingSd_, timeoutMs);
    if (!guard.acquired()) {
        return profileResult(ProfileStorageStatus::Busy, "Profile storage busy");
    }
    const ProfileListResult catalog = listProfilesUnlocked();
    if (!catalog.success()) {
        return profileResult(catalog.status, catalog.error);
    }
    if (!allowGrandfatheredOverLimit && catalog.profiles.size() > V1_PROFILE_CATALOG_MAX_COUNT) {
        return profileResult(ProfileStorageStatus::IoError, V1_PROFILE_CATALOG_LIMIT_ERROR);
    }
    try {
#ifdef UNIT_TEST
        maybeFailAllocationForTest(V1ProfileAllocationFailurePoint::SnapshotGrowth);
#endif
        profiles.reserve(catalog.profiles.size());
        for (const String& name : catalog.profiles) {
            profiles.emplace_back();
            const ProfileOperationResult loaded = loadProfileUnlocked(name, profiles.back());
            if (!loaded.success()) {
                profiles.clear();
                return loaded;
            }
        }
    } catch (const std::bad_alloc&) {
        profiles.clear();
        return profileResult(ProfileStorageStatus::IoError, "Profile snapshot memory unavailable");
    }
    return profileResult(ProfileStorageStatus::Success);
}

void V1ProfileManager::setCurrentSettings(const uint8_t* bytes) {
    memcpy(currentSettings_.bytes, bytes, 6);
    currentValid_ = true;
}

String V1ProfileManager::settingsToJson(const V1UserSettings& s) const {
    String output;
    settingsToJson(s, output);
    return output;
}

bool V1ProfileManager::settingsToJson(const V1UserSettings& s, String& output) const {
    output = "";
    PsramJson::Document doc;

    // Raw bytes
    JsonArray bytes = doc["bytes"].to<JsonArray>();
    for (int i = 0; i < 6; i++) {
        bytes.add(s.bytes[i]);
    }

    // Human-readable
    doc["xBand"] = s.xBandEnabled();
    doc["kBand"] = s.kBandEnabled();
    doc["kaBand"] = s.kaBandEnabled();
    doc["laser"] = s.laserEnabled();
    doc["kuBand"] = s.kuBandEnabled();
    doc["euro"] = s.euroMode();
    doc["kVerifier"] = s.kVerifier();
    doc["laserRear"] = s.laserRear();
    doc["customFreqs"] = s.customFreqs();
    doc["kaAlwaysPriority"] = s.kaAlwaysPriority();
    doc["fastLaserDetect"] = s.fastLaserDetect();
    doc["kaSensitivity"] = s.kaSensitivity();
    doc["kSensitivity"] = s.kSensitivity();
    doc["xSensitivity"] = s.xSensitivity();
    doc["autoMute"] = s.autoMute();
    doc["muteToMuteVolume"] = s.muteToMuteVolume();
    doc["bogeyLockLoud"] = s.bogeyLockLoud();
    doc["muteXKRear"] = s.muteXKRear();
    doc["startupSequence"] = s.startupSequence();
    doc["restingDisplay"] = s.restingDisplay();
    doc["bsmPlus"] = s.bsmPlus();
    doc["mrct"] = s.mrct();
    doc["driveSafe3D"] = s.driveSafe3D();
    doc["driveSafe3DHD"] = s.driveSafe3DHD();
    doc["redflexHalo"] = s.redflexHalo();
    doc["redflexNK7"] = s.redflexNK7();
    doc["ekin"] = s.ekin();
    doc["photoVerifier"] = s.photoVerifier();
    doc["gatsoRT4"] = s.gatsoRT4();
    doc["photoIntersectionFilter"] = s.photoIntersectionFilter();

    const size_t expected = measureJson(doc);
    if (doc.overflowed() || expected == 0 || serializeJson(doc, output) != expected ||
        output.length() != expected) {
        output = "";
        return false;
    }
    return true;
}

bool V1ProfileManager::profileToJson(const V1Profile& profile, String& output) const {
    output = "";
    PsramJson::Document doc;
    doc["schemaVersion"] = profile.schemaVersion;
    doc["name"] = profile.name;
    doc["description"] = profile.description;
    if (profile.schemaVersion == V1_PROFILE_SCHEMA_VERSION) {
        appendV1DetectorConfiguration(doc["detector"].to<JsonObject>(), profile.detector);
    } else {
        doc["legacy"] = true;
    }

    JsonObject settings = doc["settings"].to<JsonObject>();
    const V1UserSettings& s = profile.settings;

    JsonArray bytes = settings["bytes"].to<JsonArray>();
    for (int i = 0; i < 6; i++) {
        bytes.add(s.bytes[i]);
    }

    settings["xBand"] = s.xBandEnabled();
    settings["kBand"] = s.kBandEnabled();
    settings["kaBand"] = s.kaBandEnabled();
    settings["laser"] = s.laserEnabled();
    settings["kuBand"] = s.kuBandEnabled();
    settings["euro"] = s.euroMode();
    settings["kVerifier"] = s.kVerifier();
    settings["laserRear"] = s.laserRear();
    settings["customFreqs"] = s.customFreqs();
    settings["kaAlwaysPriority"] = s.kaAlwaysPriority();
    settings["fastLaserDetect"] = s.fastLaserDetect();
    settings["kaSensitivity"] = s.kaSensitivity();
    settings["kSensitivity"] = s.kSensitivity();
    settings["xSensitivity"] = s.xSensitivity();
    settings["autoMute"] = s.autoMute();
    settings["muteToMuteVolume"] = s.muteToMuteVolume();
    settings["bogeyLockLoud"] = s.bogeyLockLoud();
    settings["muteXKRear"] = s.muteXKRear();
    settings["startupSequence"] = s.startupSequence();
    settings["restingDisplay"] = s.restingDisplay();
    settings["bsmPlus"] = s.bsmPlus();
    settings["mrct"] = s.mrct();
    settings["driveSafe3D"] = s.driveSafe3D();
    settings["driveSafe3DHD"] = s.driveSafe3DHD();
    settings["redflexHalo"] = s.redflexHalo();
    settings["redflexNK7"] = s.redflexNK7();
    settings["ekin"] = s.ekin();
    settings["photoVerifier"] = s.photoVerifier();
    settings["gatsoRT4"] = s.gatsoRT4();
    settings["photoIntersectionFilter"] = s.photoIntersectionFilter();

    const size_t expected = measureJson(doc);
    if (doc.overflowed() || expected == 0 || expected > V1_PROFILE_HTTP_SAVE_MAX_BYTES) return false;
    output.reserve(expected);
    const size_t written = serializeJson(doc, output);
    if (written != expected || output.length() != expected) {
        output = "";
        return false;
    }
    return true;
}

String V1ProfileManager::profileToJson(const V1Profile& profile) const {
    String output;
    (void)profileToJson(profile, output);
    return output;
}

bool V1ProfileManager::jsonToSettings(const String& json, V1UserSettings& settings) const {
    if (json.length() > V1_PROFILE_FILE_MAX_BYTES) {
        Serial.println("[V1Profiles] JSON too large, rejecting");
        return false;
    }
    const ExactJsonInput::Status exact = ExactJsonInput::validate(json.c_str(), json.length());
    if (exact != ExactJsonInput::Status::Ok) {
        Serial.println("[V1Profiles] JSON exact-input validation failed");
        return false;
    }
    PsramJson::Document doc;
    DeserializationError err = deserializeJson(doc, json.c_str(), json.length());
    if (doc.overflowed() || err == DeserializationError::NoMemory) {
        Serial.println("[V1Profiles] JSON parse memory unavailable");
        return false;
    }
    if (err) {
        Serial.printf("[V1Profiles] JSON parse error: %s\n", err.c_str());
        return false;
    }

    // Check if settings are nested inside a "settings" object
    JsonObject settingsObj = doc["settings"].as<JsonObject>();
    if (settingsObj.isNull()) {
        // Settings are at root level
        settingsObj = doc.as<JsonObject>();
    }

    return jsonToSettings(settingsObj, settings);
}

bool V1ProfileManager::jsonToSettings(const JsonObject& settingsObj, V1UserSettings& settings) const {
    if (!validateHumanReadableSettings(settingsObj)) return false;

    // Try raw bytes first. A present raw field must be a strict six-byte array;
    // only an absent field falls back to individual settings. Redundant
    // readable fields are accepted only when they describe those exact bytes;
    // a baseBytes overlay and an authoritative bytes array are mutually
    // exclusive.
    const JsonVariantConst rawBytes = settingsObj["bytes"];
    if (!rawBytes.isUnbound()) {
        uint8_t parsedBytes[V1SettingsJson::kSettingsByteCount];
        if (!V1SettingsJson::parseRawBytes(rawBytes, parsedBytes) ||
            !settingsObj["baseBytes"].isUnbound()) {
            Serial.println("[V1Profiles] Invalid raw settings bytes");
            return false;
        }
        V1UserSettings parsed;
        memcpy(parsed.bytes, parsedBytes, sizeof(parsedBytes));
        V1UserSettings readable = parsed;
        bool anyReadableField = false;
        applyHumanReadableSettings(settingsObj, readable, anyReadableField);
        if (std::memcmp(readable.bytes, parsed.bytes, sizeof(parsed.bytes)) != 0) {
            Serial.println("[V1Profiles] Raw and readable settings conflict");
            return false;
        }
        settings = parsed;
        Serial.println("[V1Profiles] Loaded from raw bytes");
        return true;
    }

    // Parse individual settings over an optional exact-byte base. Captured
    // drafts use this to preserve reserved/unknown bits while known controls
    // are edited. An invalid base is never silently replaced.
    V1UserSettings parsed; // Constructor initializes all six bytes to 0xFF.
    const JsonVariantConst baseBytes = settingsObj["baseBytes"];
    if (!baseBytes.isUnbound()) {
        uint8_t parsedBase[V1SettingsJson::kSettingsByteCount];
        if (!V1SettingsJson::parseRawBytes(baseBytes, parsedBase)) {
            Serial.println("[V1Profiles] Invalid base settings bytes");
            return false;
        }
        memcpy(parsed.bytes, parsedBase, sizeof(parsedBase));
    }
    Serial.println("[V1Profiles] Parsing individual settings");
    bool anyField = false;
    applyHumanReadableSettings(settingsObj, parsed, anyField);

    if (!anyField) {
        Serial.println("[V1Profiles] No settings provided");
        return false;
    }

    settings = parsed;

    Serial.printf("[V1Profiles] After parse - byte0=%02X byte2=%02X\n", settings.bytes[0], settings.bytes[2]);
    Serial.printf("[V1Profiles]   xBand=%d, restingDisplay=%d, bsmPlus=%d\n", settings.xBandEnabled(),
                  settings.restingDisplay(), settings.bsmPlus());

    return true;
}
