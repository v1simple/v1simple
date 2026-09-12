#include "usb_profile_document.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <initializer_list>
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

bool canonicalName(JsonVariantConst value, String& canonical, bool allowEmpty = false) {
    if (!stringWithin(value, MAX_PROFILE_NAME_LEN)) return false;
    const String raw = value.as<String>();
    if (allowEmpty && raw.length() == 0) {
        canonical = "";
        return true;
    }
    return canonicalizeProfileName(raw, canonical) == ProfileNameStatus::Valid && canonical == raw;
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
        (root["version"].as<int>() != 1 && root["version"].as<int>() != 2) ||
        !root["autoPushEnabled"].is<bool>() || !integerWithin(root["activeSlot"], 2) ||
        !root["slots"].is<JsonArrayConst>() || root["slots"].size() != 3 ||
        !root["profiles"].is<JsonArrayConst>()) return false;

    const bool schemaV2 = root["version"].as<int>() == 2;
    target["autoPushEnabled"] = root["autoPushEnabled"];
    target["activeSlot"] = root["activeSlot"];
    target["autoPushProfileSchemaVersion"] = schemaV2 ? V1_PROFILE_SCHEMA_VERSION : 0;
    JsonArray restoredProfiles = target["profiles"].to<JsonArray>();
    std::vector<String> names;
    for (JsonVariantConst value : root["profiles"].as<JsonArrayConst>()) {
        const JsonObjectConst profile = value.as<JsonObjectConst>();
        String name;
        uint8_t bytes[V1SettingsJson::kSettingsByteCount];
        const bool validShape = schemaV2
                                    ? exactKeys(profile, {"schemaVersion", "name", "description", "rawBytes", "detector"})
                                    : exactKeys(profile, {"name", "description", "rawBytes", "displayOn", "mainVolume", "mutedVolume"});
        V1DetectorConfiguration detector;
        if (!validShape || !canonicalName(profile["name"], name) ||
            !stringWithin(profile["description"], kUsbProfileDocumentMaxBytes) ||
            !V1SettingsJson::parseRawBytes(profile["rawBytes"], bytes) ||
            (schemaV2 && (!profile["schemaVersion"].is<int>() ||
                          profile["schemaVersion"].as<int>() != V1_PROFILE_SCHEMA_VERSION ||
                          !profile["detector"].is<JsonObjectConst>() ||
                          !parseV1DetectorConfiguration(profile["detector"].as<JsonObjectConst>(), detector))) ||
            (!schemaV2 && (!profile["displayOn"].is<bool>() || !profileVolume(profile["mainVolume"]) ||
                           !profileVolume(profile["mutedVolume"])))) {
            error = "Invalid profile fields";
            return false;
        }
        for (const String& prior : names) {
            if (profileCanonicalCollisionKey(prior) == profileCanonicalCollisionKey(name)) {
                error = "Duplicate profile name";
                return false;
            }
        }
        names.push_back(name);
        JsonObject restored = restoredProfiles.add<JsonObject>();
        restored["name"] = name;
        restored["description"] = profile["description"];
        if (schemaV2) {
            restored["schemaVersion"] = V1_PROFILE_SCHEMA_VERSION;
            appendV1DetectorConfiguration(restored["detector"].to<JsonObject>(), detector);
        } else {
            restored["displayOn"] = profile["displayOn"];
            restored["mainVolume"] = profile["mainVolume"];
            restored["mutedVolume"] = profile["mutedVolume"];
        }
        restored["bytes"] = profile["rawBytes"];
    }

    int index = 0;
    for (JsonVariantConst value : root["slots"].as<JsonArrayConst>()) {
        const JsonObjectConst slot = value.as<JsonObjectConst>();
        String profile;
        const bool validSlotShape = schemaV2
                                        ? exactKeys(slot, {"name", "profile", "color", "alertPersist", "priorityArrowOnly"})
                                        : exactKeys(slot, {"name", "profile", "mode", "color", "volumeConfigured", "volume", "muteVolume",
                                                           "darkMode", "muteToZero", "alertPersist", "priorityArrowOnly"});
        if (!validSlotShape ||
            !stringWithin(slot["name"], MAX_SLOT_NAME_LEN) ||
            sanitizeSlotNameValue(slot["name"].as<String>()) != slot["name"].as<String>() ||
            !canonicalName(slot["profile"], profile, true) ||
            !integerWithin(slot["color"], 65535) ||
            !integerWithin(slot["alertPersist"], 5) || !slot["priorityArrowOnly"].is<bool>() ||
            (!schemaV2 && (!integerWithin(slot["mode"], 3) || !slot["volumeConfigured"].is<bool>() ||
                           !integerWithin(slot["volume"], 9) || !integerWithin(slot["muteVolume"], 9) ||
                           !slot["darkMode"].is<bool>() || !slot["muteToZero"].is<bool>() ||
                           (!slot["volumeConfigured"].as<bool>() &&
                            (slot["volume"].as<int>() != 0 || slot["muteVolume"].as<int>() != 0))))) {
            error = "Invalid Auto-Push slot fields";
            return false;
        }
        bool assignedFound = profile.length() == 0;
        for (const String& name : names) assignedFound |= name == profile;
        if (!assignedFound) {
            error = "Slot refers to a profile absent from this document";
            return false;
        }
        const auto copy = [&](const char* suffix, JsonVariantConst input) {
            char key[32];
            std::snprintf(key, sizeof(key), "slot%d%s", index, suffix);
            target[key] = input;
        };
        copy("Name", slot["name"]);
        copy("ProfileName", slot["profile"]);
        if (schemaV2) {
            char key[32];
            std::snprintf(key, sizeof(key), "slot%dMode", index);
            target[key] = 0;
        } else {
            copy("Mode", slot["mode"]);
        }
        copy("Color", slot["color"]);
        if (schemaV2) {
            char darkKey[32], muteZeroKey[32];
            std::snprintf(darkKey, sizeof(darkKey), "slot%dDarkMode", index);
            std::snprintf(muteZeroKey, sizeof(muteZeroKey), "slot%dMuteToZero", index);
            target[darkKey] = false;
            target[muteZeroKey] = false;
        } else {
            copy("DarkMode", slot["darkMode"]);
            copy("MuteToZero", slot["muteToZero"]);
        }
        copy("AlertPersist", slot["alertPersist"]);
        copy("PriorityArrow", slot["priorityArrowOnly"]);
        char volumeKey[24], muteKey[24];
        std::snprintf(volumeKey, sizeof(volumeKey), "slot%dVolume", index);
        std::snprintf(muteKey, sizeof(muteKey), "slot%dMuteVolume", index);
        target[volumeKey] = schemaV2 ? 255 :
                            (slot["volumeConfigured"].as<bool>() ? slot["volume"].as<int>() : 255);
        target[muteKey] = schemaV2 ? 255 :
                          (slot["volumeConfigured"].as<bool>() ? slot["muteVolume"].as<int>() : 255);
        ++index;
    }
    if (target.overflowed()) {
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
    doc["version"] = 2;
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
