#include "usb_profile_document.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <new>
#include <utility>
#include <vector>

#include "settings_sanitize.h"
#include "v1_profiles.h"
#include "v1_settings_json.h"

namespace {

bool exactKeys(JsonObjectConst object, std::initializer_list<const char*> keys) {
    if (object.isNull() || object.size() != keys.size()) return false;
    for (const char* key : keys) {
        if (object[key].isUnbound()) return false;
    }
    return true;
}

bool stringWithin(JsonVariantConst value, size_t limit) {
    if (!value.is<const char*>()) return false;
    const JsonString text = value.as<JsonString>();
    return text.size() <= limit && std::strlen(text.c_str()) == text.size();
}

bool integerWithin(JsonVariantConst value, int maximum) {
    return value.is<int>() && value.as<int>() >= 0 && value.as<int>() <= maximum;
}

bool profileVolume(JsonVariantConst value) {
    return integerWithin(value, 9) || (value.is<int>() && value.as<int>() == 255);
}

enum class UsbInputStringField : uint8_t {
    ProfileName = 0,
    ProfileDescription,
    SlotName,
    SlotProfile,
};

#ifdef UNIT_TEST
UsbInputStringField g_failUsbInputStringCopyForTest = UsbInputStringField::ProfileName;
bool g_failUsbInputStringCopyEnabledForTest = false;
bool g_failPreparedUsbProfileAllocationForTest = false;
#endif

ExactV1JsonStringStatus checkedUsbInputString(JsonVariantConst value, String& output,
                                              size_t maxBytes, UsbInputStringField field) {
#ifdef UNIT_TEST
    if (g_failUsbInputStringCopyEnabledForTest && g_failUsbInputStringCopyForTest == field) {
        output = String();
        return ExactV1JsonStringStatus::Unavailable;
    }
#endif
    return exactV1JsonStringChecked(value, output, maxBytes);
}

bool profileNameAlreadyCanonical(const String& value) {
    if (value.length() == 0 || value.length() > MAX_PROFILE_NAME_LEN ||
        value[0] == '.' || value[0] == '_' || value[0] == ' ' ||
        value[value.length() - 1u] == ' ' || value.indexOf('/') >= 0 ||
        value.indexOf('\\') >= 0 || value.indexOf("..") >= 0) {
        return false;
    }
    for (size_t index = 0; index < value.length(); ++index) {
        const unsigned char byte = static_cast<unsigned char>(value[index]);
        if (byte < 0x20u || byte == 0x7fu || byte == ':' || byte == '*' ||
            byte == '?' || byte == '"' || byte == '<' || byte == '>' || byte == '|') {
            return false;
        }
    }
    return true;
}

ExactV1JsonStringStatus canonicalNameChecked(JsonVariantConst value, String& canonical,
                                             UsbInputStringField field, bool allowEmpty = false) {
    String raw;
    const ExactV1JsonStringStatus copyStatus =
        checkedUsbInputString(value, raw, MAX_PROFILE_NAME_LEN, field);
    if (copyStatus != ExactV1JsonStringStatus::Valid) return copyStatus;
    if (allowEmpty && raw.length() == 0) {
        canonical = std::move(raw);
        return ExactV1JsonStringStatus::Valid;
    }
    // The source copy above is the only allocation. Validate the exact
    // canonical representation in place so a second String allocation cannot
    // turn an otherwise valid name into a misleading semantic failure.
    if (!profileNameAlreadyCanonical(raw)) return ExactV1JsonStringStatus::Invalid;
    canonical = std::move(raw);
    return ExactV1JsonStringStatus::Valid;
}

bool slotNameAlreadySanitized(const String& value) {
    // sanitizeSlotNameValue() only byte-clamps (already checked above) and
    // uppercases ASCII. Compare that canonical condition in place so
    // validation does not need a second allocation-prone String temporary.
    for (size_t index = 0; index < value.length(); ++index) {
        const unsigned char byte = static_cast<unsigned char>(value[index]);
        if (byte >= 'a' && byte <= 'z') return false;
    }
    return true;
}

struct PreparedUsbProfile {
    String name;
    String description;
    uint8_t bytes[V1SettingsJson::kSettingsByteCount] = {};
    V1DetectorConfiguration detector;
    bool displayOn = false;
    uint8_t mainVolume = 0xFF;
    uint8_t mutedVolume = 0xFF;
};

struct PreparedUsbSlot {
    String name;
    String profile;
    uint8_t mode = 0;
    uint16_t color = 0;
    uint8_t volume = 0;
    uint8_t muteVolume = 0;
    bool volumeConfigured = false;
    bool darkMode = false;
    bool muteToZero = false;
    uint8_t alertPersist = 0;
    bool priorityArrowOnly = false;
};

bool reportStringStatus(ExactV1JsonStringStatus status, String& error, const char* invalidError) {
    if (status == ExactV1JsonStringStatus::Valid) return true;
    error = status == ExactV1JsonStringStatus::Unavailable
                ? "Could not allocate exact profile import string"
                : invalidError;
    return false;
}

struct LegacyApplicationSignature {
    int sourceIndex = -1;
    uint8_t bytes[V1SettingsJson::kSettingsByteCount] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    uint8_t mode = 0;
    uint8_t mainVolume = 0xFF;
    uint8_t mutedVolume = 0xFF;
    bool darkMode = false;
};

bool sameLegacyApplication(const LegacyApplicationSignature& lhs,
                           const LegacyApplicationSignature& rhs) {
    return lhs.sourceIndex == rhs.sourceIndex && lhs.mode == rhs.mode &&
           lhs.mainVolume == rhs.mainVolume && lhs.mutedVolume == rhs.mutedVolume &&
           lhs.darkMode == rhs.darkMode &&
           std::memcmp(lhs.bytes, rhs.bytes, sizeof(lhs.bytes)) == 0;
}

bool legacyMigrationFitsCatalogCap(JsonObjectConst root) {
    const JsonArrayConst profiles = root["profiles"].as<JsonArrayConst>();
    const JsonArrayConst slots = root["slots"].as<JsonArrayConst>();
    size_t projectedCount = profiles.size();
    std::array<LegacyApplicationSignature, 3> variants{};
    size_t variantCount = 0;

    for (JsonVariantConst slotValue : slots) {
        const JsonObjectConst slot = slotValue.as<JsonObjectConst>();
        const JsonString assigned = slot["profile"].as<JsonString>();
        LegacyApplicationSignature application;
        if (assigned.size() != 0) {
            int candidateIndex = 0;
            for (JsonVariantConst profileValue : profiles) {
                const JsonObjectConst profile = profileValue.as<JsonObjectConst>();
                const JsonString name = profile["name"].as<JsonString>();
                if (name.size() == assigned.size() &&
                    std::memcmp(name.c_str(), assigned.c_str(), name.size()) == 0) {
                    application.sourceIndex = candidateIndex;
                    if (!V1SettingsJson::parseRawBytes(profile["rawBytes"], application.bytes)) return false;
                    break;
                }
                ++candidateIndex;
            }
            if (application.sourceIndex < 0) return false;
            if (slot["muteToZero"].as<bool>()) {
                application.bytes[0] &= static_cast<uint8_t>(~0x10u);
            } else {
                application.bytes[0] |= 0x10u;
            }
        }
        application.mode = slot["mode"].as<uint8_t>();
        application.darkMode = slot["darkMode"].as<bool>();
        if (slot["volumeConfigured"].as<bool>()) {
            application.mainVolume = slot["volume"].as<uint8_t>();
            application.mutedVolume = slot["muteVolume"].as<uint8_t>();
        }

        bool duplicate = false;
        bool firstForSource = true;
        for (size_t index = 0; index < variantCount; ++index) {
            const LegacyApplicationSignature& prior = variants[index];
            if (sameLegacyApplication(prior, application)) duplicate = true;
            if (prior.sourceIndex == application.sourceIndex) firstForSource = false;
        }
        if (duplicate) continue;
        if (application.sourceIndex < 0 || !firstForSource) ++projectedCount;
        if (projectedCount > V1_PROFILE_CATALOG_MAX_COUNT) return false;
        if (variantCount >= variants.size()) return false;
        variants[variantCount++] = application;
    }
    return true;
}

bool toRestoreDocument(const JsonDocument& source, JsonDocument& target, String& error) {
    error = "Invalid profile document";
    target.clear();
    if (source.overflowed() || measureJson(source) > kUsbProfileDocumentMaxBytes) {
        error = "Profile document exceeds 128 KiB or could not be allocated";
        return false;
    }
    const JsonObjectConst root = source.as<JsonObjectConst>();
    if (!exactKeys(root, {"format", "version", "autoPushEnabled", "activeSlot", "slots", "profiles"}) ||
        !stringWithin(root["format"], 17) ||
        std::strcmp(root["format"].as<const char*>(), "v1simple-profiles") != 0 ||
        !root["version"].is<int>() ||
        (root["version"].as<int>() != 1 && root["version"].as<int>() != 2 &&
         root["version"].as<int>() != V1_PROFILE_SCHEMA_VERSION) ||
        !root["autoPushEnabled"].is<bool>() || !integerWithin(root["activeSlot"], 2) ||
        !root["slots"].is<JsonArrayConst>() || root["slots"].size() != 3 ||
        !root["profiles"].is<JsonArrayConst>() ||
        root["profiles"].size() > V1_PROFILE_CATALOG_MAX_COUNT) {
        if (root["profiles"].is<JsonArrayConst>() &&
            root["profiles"].size() > V1_PROFILE_CATALOG_MAX_COUNT) {
            error = V1_PROFILE_CATALOG_LIMIT_ERROR;
        }
        return false;
    }

    const int documentVersion = root["version"].as<int>();
    const bool versioned = documentVersion >= V1_PROFILE_PREVIOUS_SCHEMA_VERSION;
    std::unique_ptr<PreparedUsbProfile[]> preparedProfiles;
#ifdef UNIT_TEST
    if (g_failPreparedUsbProfileAllocationForTest) {
        g_failPreparedUsbProfileAllocationForTest = false;
    } else
#endif
    {
        preparedProfiles.reset(
            new (std::nothrow) PreparedUsbProfile[V1_PROFILE_CATALOG_MAX_COUNT]);
    }
    if (!preparedProfiles) {
        error = "Could not allocate profile import staging";
        return false;
    }
    size_t preparedProfileCount = 0;
    for (JsonVariantConst value : root["profiles"].as<JsonArrayConst>()) {
        const JsonObjectConst profile = value.as<JsonObjectConst>();
        PreparedUsbProfile& prepared = preparedProfiles[preparedProfileCount];
        const bool validShape = versioned
                                    ? exactKeys(profile, {"schemaVersion", "name", "description", "rawBytes", "detector"})
                                    : exactKeys(profile, {"name", "description", "rawBytes", "displayOn", "mainVolume", "mutedVolume"});
        if (!validShape) {
            error = "Invalid profile fields";
            return false;
        }
        if (!reportStringStatus(
                canonicalNameChecked(profile["name"], prepared.name,
                                     UsbInputStringField::ProfileName),
                error, "Invalid profile fields") ||
            !reportStringStatus(
                checkedUsbInputString(profile["description"], prepared.description,
                                      V1_PROFILE_DESCRIPTION_MAX_BYTES,
                                      UsbInputStringField::ProfileDescription),
                error, "Invalid profile fields")) {
            return false;
        }
        if (!V1SettingsJson::parseRawBytes(profile["rawBytes"], prepared.bytes) ||
            (versioned && (!profile["schemaVersion"].is<int>() ||
                           profile["schemaVersion"].as<int>() != documentVersion ||
                           !profile["detector"].is<JsonObjectConst>() ||
                           !(documentVersion == V1_PROFILE_SCHEMA_VERSION
                                 ? parseV1DetectorConfiguration(profile["detector"].as<JsonObjectConst>(), prepared.detector)
                                 : parseV1DetectorConfigurationV2(profile["detector"].as<JsonObjectConst>(), prepared.detector)))) ||
            (!versioned && (!profile["displayOn"].is<bool>() || !profileVolume(profile["mainVolume"]) ||
                           !profileVolume(profile["mutedVolume"])))) {
            error = "Invalid profile fields";
            return false;
        }
        if (versioned && documentVersion == V1_PROFILE_PREVIOUS_SCHEMA_VERSION) {
            prepared.detector = migrateV1DetectorConfigurationV2(prepared.detector);
        } else if (!versioned) {
            prepared.displayOn = profile["displayOn"].as<bool>();
            prepared.mainVolume = profile["mainVolume"].as<uint8_t>();
            prepared.mutedVolume = profile["mutedVolume"].as<uint8_t>();
        }
        for (size_t prior = 0; prior < preparedProfileCount; ++prior) {
            if (profileCanonicalNamesCollide(preparedProfiles[prior].name, prepared.name)) {
                error = "Duplicate profile name";
                return false;
            }
        }
        ++preparedProfileCount;
    }

    std::array<PreparedUsbSlot, 3> preparedSlots;
    size_t slotIndex = 0;
    for (JsonVariantConst value : root["slots"].as<JsonArrayConst>()) {
        const JsonObjectConst slot = value.as<JsonObjectConst>();
        PreparedUsbSlot& prepared = preparedSlots[slotIndex];
        const bool validSlotShape = versioned
                                        ? exactKeys(slot, {"name", "profile", "color", "alertPersist", "priorityArrowOnly"})
                                        : exactKeys(slot, {"name", "profile", "mode", "color", "volumeConfigured", "volume", "muteVolume",
                                                           "darkMode", "muteToZero", "alertPersist", "priorityArrowOnly"});
        if (!validSlotShape) {
            error = "Invalid Auto-Push slot fields";
            return false;
        }
        if (!reportStringStatus(
                checkedUsbInputString(slot["name"], prepared.name, MAX_SLOT_NAME_LEN,
                                      UsbInputStringField::SlotName),
                error, "Invalid Auto-Push slot fields") ||
            !reportStringStatus(
                canonicalNameChecked(slot["profile"], prepared.profile,
                                     UsbInputStringField::SlotProfile, true),
                error, "Invalid Auto-Push slot fields")) {
            return false;
        }
        if (!slotNameAlreadySanitized(prepared.name) ||
            !integerWithin(slot["color"], 65535) ||
            (documentVersion == V1_PROFILE_SCHEMA_VERSION && slot["color"].as<int>() == 0) ||
            !integerWithin(slot["alertPersist"], 5) || !slot["priorityArrowOnly"].is<bool>() ||
            (!versioned && (!integerWithin(slot["mode"], 3) || !slot["volumeConfigured"].is<bool>() ||
                           !integerWithin(slot["volume"], 9) || !integerWithin(slot["muteVolume"], 9) ||
                           !slot["darkMode"].is<bool>() || !slot["muteToZero"].is<bool>() ||
                           (!slot["volumeConfigured"].as<bool>() &&
                            (slot["volume"].as<int>() != 0 || slot["muteVolume"].as<int>() != 0))))) {
            error = "Invalid Auto-Push slot fields";
            return false;
        }
        bool assignedFound = prepared.profile.length() == 0;
        for (size_t profileIndex = 0; profileIndex < preparedProfileCount; ++profileIndex) {
            assignedFound |= preparedProfiles[profileIndex].name == prepared.profile;
        }
        if (!assignedFound) {
            error = "Slot refers to a profile absent from this document";
            return false;
        }
        prepared.color = slot["color"].as<uint16_t>();
        prepared.alertPersist = slot["alertPersist"].as<uint8_t>();
        prepared.priorityArrowOnly = slot["priorityArrowOnly"].as<bool>();
        if (!versioned) {
            prepared.mode = slot["mode"].as<uint8_t>();
            prepared.volumeConfigured = slot["volumeConfigured"].as<bool>();
            prepared.volume = slot["volume"].as<uint8_t>();
            prepared.muteVolume = slot["muteVolume"].as<uint8_t>();
            prepared.darkMode = slot["darkMode"].as<bool>();
            prepared.muteToZero = slot["muteToZero"].as<bool>();
        }
        ++slotIndex;
    }

    if (!versioned && !legacyMigrationFitsCatalogCap(root)) {
        error = "Legacy profile migration exceeds the supported 10-profile catalog";
        return false;
    }

    // All source strings, references, and detector configurations are now
    // exact and owned. Only after that proof do we begin constructing the
    // document that the transactional restore layer may consume.
    target["autoPushEnabled"] = root["autoPushEnabled"].as<bool>();
    target["activeSlot"] = root["activeSlot"].as<uint8_t>();
    target["autoPushProfileSchemaVersion"] = versioned ? V1_PROFILE_SCHEMA_VERSION : 0;
    JsonArray restoredProfiles = target["profiles"].to<JsonArray>();
    for (size_t index = 0; index < preparedProfileCount; ++index) {
        const PreparedUsbProfile& prepared = preparedProfiles[index];
        JsonObject restored = restoredProfiles.add<JsonObject>();
        restored["name"] = prepared.name;
        restored["description"] = prepared.description;
        if (versioned) {
            restored["schemaVersion"] = V1_PROFILE_SCHEMA_VERSION;
            appendV1DetectorConfiguration(restored["detector"].to<JsonObject>(), prepared.detector);
        } else {
            restored["displayOn"] = prepared.displayOn;
            restored["mainVolume"] = prepared.mainVolume;
            restored["mutedVolume"] = prepared.mutedVolume;
        }
        JsonArray bytes = restored["bytes"].to<JsonArray>();
        for (uint8_t byte : prepared.bytes) bytes.add(byte);
    }

    for (size_t index = 0; index < preparedSlots.size(); ++index) {
        const PreparedUsbSlot& prepared = preparedSlots[index];
        const auto copyValue = [&](const char* suffix, const auto& input) {
            char key[32];
            std::snprintf(key, sizeof(key), "slot%u%s", static_cast<unsigned>(index), suffix);
            target[key] = input;
        };
        copyValue("Name", prepared.name);
        copyValue("ProfileName", prepared.profile);
        if (versioned) {
            char key[32];
            std::snprintf(key, sizeof(key), "slot%uMode", static_cast<unsigned>(index));
            target[key] = 0;
        } else {
            copyValue("Mode", prepared.mode);
        }
        copyValue("Color", prepared.color);
        if (versioned) {
            char darkKey[32], muteZeroKey[32];
            std::snprintf(darkKey, sizeof(darkKey), "slot%uDarkMode", static_cast<unsigned>(index));
            std::snprintf(muteZeroKey, sizeof(muteZeroKey), "slot%uMuteToZero", static_cast<unsigned>(index));
            target[darkKey] = false;
            target[muteZeroKey] = false;
        } else {
            copyValue("DarkMode", prepared.darkMode);
            copyValue("MuteToZero", prepared.muteToZero);
        }
        copyValue("AlertPersist", prepared.alertPersist);
        copyValue("PriorityArrow", prepared.priorityArrowOnly);
        char volumeKey[24], muteKey[24];
        std::snprintf(volumeKey, sizeof(volumeKey), "slot%uVolume", static_cast<unsigned>(index));
        std::snprintf(muteKey, sizeof(muteKey), "slot%uMuteVolume", static_cast<unsigned>(index));
        target[volumeKey] = versioned ? 255 : (prepared.volumeConfigured ? prepared.volume : 255);
        target[muteKey] = versioned ? 255 : (prepared.volumeConfigured ? prepared.muteVolume : 255);
    }
    if (target.overflowed()) {
        target.clear();
        error = "Could not allocate profile restore document";
        return false;
    }
    error = "";
    return true;
}

} // namespace

bool buildUsbProfileDocument(JsonDocument& doc, SettingsManager& settings, V1ProfileManager& profiles, String& error) {
    doc.clear();
    error = "";
    if (!settings.resolveStorageTransactionsForMutation()) {
        error = "Profile export unavailable while storage recovery is pending";
        return false;
    }
    if (settings.get().autoPushProfileSchemaVersion != V1_PROFILE_SCHEMA_VERSION) {
        error = "Profile export unavailable until detector-profile migration completes";
        return false;
    }
    std::vector<V1Profile> snapshot;
    const ProfileOperationResult result = profiles.snapshotProfiles(snapshot, 250);
    if (!result.success()) {
        error = result.error.length() ? result.error : "Profile catalog unavailable";
        return false;
    }
    for (const V1Profile& profile : snapshot) {
        if (profile.schemaVersion != V1_PROFILE_SCHEMA_VERSION) {
            error = "Profile export unavailable because a legacy profile remains after migration";
            return false;
        }
    }
    std::sort(snapshot.begin(), snapshot.end(), [](const V1Profile& a, const V1Profile& b) {
        return std::strcmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    doc["format"] = "v1simple-profiles";
    doc["version"] = V1_PROFILE_SCHEMA_VERSION;
    const V1Settings& state = settings.get();
    doc["autoPushEnabled"] = state.autoPushEnabled;
    doc["activeSlot"] = state.activeSlot;
    JsonArray slots = doc["slots"].to<JsonArray>();
    for (int index = 0; index < 3; ++index) {
        const auto slot = state.autoPushSlotView(index);
        JsonObject output = slots.add<JsonObject>();
        output["name"] = slot.name;
        output["profile"] = slot.config.profileName;
        output["color"] = slot.color;
        output["alertPersist"] = slot.alertPersist;
        output["priorityArrowOnly"] = slot.priorityArrow;
    }
    JsonArray outputProfiles = doc["profiles"].to<JsonArray>();
    for (const V1Profile& profile : snapshot) {
        JsonObject output = outputProfiles.add<JsonObject>();
        output["schemaVersion"] = V1_PROFILE_SCHEMA_VERSION;
        output["name"] = profile.name;
        output["description"] = profile.description;
        appendV1DetectorConfiguration(output["detector"].to<JsonObject>(), profile.detector);
        JsonArray bytes = output["rawBytes"].to<JsonArray>();
        for (uint8_t byte : profile.settings.bytes) bytes.add(byte);
    }
    JsonDocument validated(doc.allocator());
    if (!toRestoreDocument(doc, validated, error)) {
        doc.clear();
        return false;
    }
    return true;
}

SettingsBackupApplyResult applyUsbProfileDocument(SettingsManager& settings, V1ProfileManager& profiles,
                                                  const JsonDocument& doc, String& error,
                                                  const SettingsRestoreWatchdog& watchdog) {
    JsonDocument restore(doc.allocator());
    if (!toRestoreDocument(doc, restore, error)) return {};
    if (!profiles.isReady()) {
        error = "Profile storage unavailable";
        return {};
    }
    SettingsBackupApplyResult result =
        settings.applyBackupDocument(restore, true, watchdog, SettingsBackupScope::ProfilesOnly);
    if (!result.success) {
        error = "Profile restore did not commit; storage recovery may be required";
        return result;
    }
    if (doc["version"].is<int>() && doc["version"].as<int>() == 1 &&
        !settings.migrateAutoPushProfilesToV2()) {
        result.migrationPending = true;
        error = "Legacy profile restore committed safely, but detector-profile migration is pending";
    }
    return result;
}
