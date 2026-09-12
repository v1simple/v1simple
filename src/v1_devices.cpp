#include "v1_devices.h"
#include "storage_json_rollback.h"
#include "storage_manager.h"

#include <ArduinoJson.h>

#include <algorithm>
#include <cstring>

namespace {

constexpr const char* STORE_PATH = "/v1devices.json";
constexpr const char* STORE_TMP_PATH = "/v1devices.tmp";
constexpr const char* LEGACY_ADDR_PATH = "/known_v1.txt";
constexpr const char* LEGACY_NAME_PATH = "/known_v1_names.txt";
constexpr const char* LEGACY_PROFILE_PATH = "/known_v1_profiles.txt";
constexpr uint8_t STORE_VERSION = 3;
constexpr uint8_t FIRST_CHECKSUM_STORE_VERSION = 2;

bool isHex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

String clampLen(const String& input, size_t maxLen) {
    if (input.length() <= maxLen) {
        return input;
    }
    return input.substring(0, maxLen);
}

uint32_t deviceStoreCrc32(const uint8_t* data, size_t length) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

uint32_t deviceStoreContentCrc(const JsonDocument& doc) {
    JsonDocument integrity;
    integrity["version"] = doc["version"] | STORE_VERSION;
    integrity["generation"] = doc["generation"] | 0u;
    integrity["devices"].set(doc["devices"].as<JsonVariantConst>());
    String serialized;
    serializeJson(integrity, serialized);
    return deviceStoreCrc32(reinterpret_cast<const uint8_t*>(serialized.c_str()), serialized.length());
}

bool loadDeviceStoreRollbackDocument(fs::FS& filesystem, size_t maxBytes, JsonDocument& doc) {
    const String rollbackPath = StorageManager::rollbackPathFor(STORE_PATH);
    if (rollbackPath.length() == 0 || !filesystem.exists(rollbackPath.c_str())) return false;

    File file = filesystem.open(rollbackPath.c_str(), FILE_READ);
    if (!file) return false;
    const size_t fileSize = file.size();
    if (fileSize == 0 || fileSize > maxBytes) {
        file.close();
        return false;
    }

    doc.clear();
    const DeserializationError error = deserializeJson(doc, file);
    file.close();
    if (error) {
        doc.clear();
        return false;
    }
    return true;
}

int parseDefaultProfile(const String& raw) {
    String value = raw;
    value.trim();
    if (value.length() == 0) {
        return 0;
    }
    return value.toInt();
}

void appendSnapshot(JsonObject target, const V1DetectorSnapshot& snapshot) {
    target["capturedBootId"] = snapshot.capturedBootId;
    target["capturedUptimeMs"] = snapshot.capturedUptimeMs;
    target["sessionGeneration"] = snapshot.sessionGeneration;
    target["captureTimedOut"] = snapshot.captureTimedOut;
    if (snapshot.hasFirmwareVersion) target["firmwareVersion"] = snapshot.firmwareVersion;
    if (snapshot.hasUserBytes) {
        JsonArray bytes = target["userBytes"].to<JsonArray>();
        for (uint8_t value : snapshot.userBytes) bytes.add(value);
    }
    if (snapshot.hasMode) {
        // Store the byte value instead of a pointer to stack-backed text.
        target["mode"] = static_cast<uint8_t>(snapshot.mode);
    }
    if (snapshot.hasDisplayOn) target["displayOn"] = snapshot.displayOn;
    if (snapshot.hasCurrentVolume) {
        JsonObject volume = target["currentVolume"].to<JsonObject>();
        volume["main"] = snapshot.currentMainVolume;
        volume["muted"] = snapshot.currentMutedVolume;
    }
    if (snapshot.hasSavedVolume) {
        JsonObject volume = target["savedVolume"].to<JsonObject>();
        volume["main"] = snapshot.savedMainVolume;
        volume["muted"] = snapshot.savedMutedVolume;
    }
}

bool parseVolumePair(JsonVariantConst source, uint8_t& main, uint8_t& muted) {
    if (!source.is<JsonObjectConst>() || !source["main"].is<int>() || !source["muted"].is<int>()) return false;
    const int parsedMain = source["main"].as<int>();
    const int parsedMuted = source["muted"].as<int>();
    if (parsedMain < 0 || parsedMain > 9 || parsedMuted < 0 || parsedMuted > 9) return false;
    main = static_cast<uint8_t>(parsedMain);
    muted = static_cast<uint8_t>(parsedMuted);
    return true;
}

V1DetectorSnapshot parseSnapshot(JsonVariantConst source) {
    V1DetectorSnapshot snapshot;
    if (!source.is<JsonObjectConst>()) return snapshot;
    snapshot.available = true;
    snapshot.capturedBootId = source["capturedBootId"] | 0u;
    snapshot.capturedUptimeMs = source["capturedUptimeMs"] | 0u;
    snapshot.sessionGeneration = source["sessionGeneration"] | 0u;
    snapshot.captureTimedOut = source["captureTimedOut"] | false;

    if (source["firmwareVersion"].is<uint32_t>()) {
        snapshot.firmwareVersion = source["firmwareVersion"].as<uint32_t>();
        snapshot.hasFirmwareVersion = snapshot.firmwareVersion != 0;
    }

    JsonArrayConst bytes = source["userBytes"].as<JsonArrayConst>();
    if (!bytes.isNull() && bytes.size() == snapshot.userBytes.size()) {
        bool valid = true;
        size_t index = 0;
        for (JsonVariantConst value : bytes) {
            if (!value.is<int>() || value.as<int>() < 0 || value.as<int>() > 255) {
                valid = false;
                break;
            }
            snapshot.userBytes[index++] = static_cast<uint8_t>(value.as<int>());
        }
        snapshot.hasUserBytes = valid;
    }

    if (source["mode"].is<uint8_t>()) {
        snapshot.mode = static_cast<char>(source["mode"].as<uint8_t>());
        snapshot.hasMode = true;
    } else {
        // Accept early development snapshots that encoded the mode as text.
        const char* mode = source["mode"].as<const char*>();
        if (mode && mode[0] != '\0' && mode[1] == '\0') {
            snapshot.mode = mode[0];
            snapshot.hasMode = true;
        }
    }
    if (source["displayOn"].is<bool>()) {
        snapshot.displayOn = source["displayOn"].as<bool>();
        snapshot.hasDisplayOn = true;
    }
    snapshot.hasCurrentVolume = parseVolumePair(source["currentVolume"], snapshot.currentMainVolume,
                                                snapshot.currentMutedVolume);
    snapshot.hasSavedVolume =
        parseVolumePair(source["savedVolume"], snapshot.savedMainVolume, snapshot.savedMutedVolume);
    return snapshot;
}

} // namespace

String normalizeV1DeviceAddress(const String& rawAddress) {
    String value = rawAddress;
    value.trim();
    value.replace("-", ":");
    value.toUpperCase();

    if (value.length() != 17) {
        return "";
    }

    for (int i = 0; i < 17; ++i) {
        char c = value[i];
        if ((i + 1) % 3 == 0) {
            if (c != ':') {
                return "";
            }
            continue;
        }
        if (!isHex(c)) {
            return "";
        }
    }

    return value;
}

V1DeviceStore::V1DeviceStore() = default;

String V1DeviceStore::sanitizeName(const String& raw) {
    String name = clampLen(raw, MAX_NAME_LEN);
    name.trim();
    return name;
}

uint8_t V1DeviceStore::clampDefaultProfileValue(int raw) {
    if (raw < 0) {
        return 0;
    }
    if (raw > 3) {
        return 3;
    }
    return static_cast<uint8_t>(raw);
}

int V1DeviceStore::findDeviceIndex(const String& normalizedAddress) const {
    for (size_t i = 0; i < devices_.size(); ++i) {
        if (devices_[i].address.equalsIgnoreCase(normalizedAddress)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void V1DeviceStore::trimToCapacity() {
    if (devices_.size() > MAX_DEVICES) {
        devices_.resize(MAX_DEVICES);
    }
}

bool V1DeviceStore::writeStore(fs::FS& filesystem, uint32_t generation, bool preserveValidRollback) const {
    JsonDocument doc;
    doc["version"] = STORE_VERSION;
    doc["generation"] = generation;
    JsonArray arr = doc["devices"].to<JsonArray>();

    for (const auto& device : devices_) {
        JsonObject obj = arr.add<JsonObject>();
        obj["address"] = device.address;
        obj["name"] = device.name;
        obj["defaultProfile"] = device.defaultProfile;
        obj["lastSeenMs"] = device.lastSeenMs;
        if (device.snapshot.available) {
            appendSnapshot(obj["snapshot"].to<JsonObject>(), device.snapshot);
        }
    }

    doc["crc32"] = deviceStoreContentCrc(doc);

    if (filesystem.exists(STORE_TMP_PATH)) {
        filesystem.remove(STORE_TMP_PATH);
    }

    File file = filesystem.open(STORE_TMP_PATH, FILE_WRITE);
    if (!file) {
        return false;
    }

    const size_t expected = measureJson(doc);
    size_t written = serializeJson(doc, file);
    file.flush();
    file.close();

    if (written != expected) {
        filesystem.remove(STORE_TMP_PATH);
        return false;
    }

    File verifyFile = filesystem.open(STORE_TMP_PATH, FILE_READ);
    JsonDocument verified;
    const bool validCandidate = verifyFile && verifyFile.size() == written && !deserializeJson(verified, verifyFile) &&
                                verified["version"].as<uint8_t>() == STORE_VERSION &&
                                verified["generation"].as<uint32_t>() == generation &&
                                verified["crc32"].is<uint32_t>() &&
                                verified["crc32"].as<uint32_t>() == deviceStoreContentCrc(verified);
    if (verifyFile) verifyFile.close();
    if (!validCandidate) {
        filesystem.remove(STORE_TMP_PATH);
        return false;
    }

    if (preserveValidRollback) {
        // readStore already proved .prev is the only valid catalog on this
        // filesystem. Do not let the generic rotation replace that good copy
        // with the known-bad live file before the new candidate is promoted.
        const String rollbackPath = StorageManager::rollbackPathFor(STORE_PATH);
        if (filesystem.exists(STORE_PATH) && !filesystem.remove(STORE_PATH)) {
            filesystem.remove(STORE_TMP_PATH);
            return false;
        }
        if (!filesystem.rename(STORE_TMP_PATH, STORE_PATH)) {
            filesystem.remove(STORE_TMP_PATH);
            return false;
        }
        if (rollbackPath.length() > 0 && filesystem.exists(rollbackPath.c_str())) {
            filesystem.remove(rollbackPath.c_str());
        }
    } else if (!StorageManager::promoteTempFileWithRollback(filesystem, STORE_TMP_PATH, STORE_PATH)) {
        return false;
    }

    return true;
}

V1DeviceStore::StoreSnapshot V1DeviceStore::readStore(fs::FS& filesystem) const {
    JsonDocument doc;
    JsonRollbackLoadResult loadResult =
        loadJsonDocumentWithRollback(filesystem, STORE_PATH, MAX_STORE_BYTES, doc);
    StoreSnapshot snapshot;
    if (loadResult == JsonRollbackLoadResult::Missing) {
        return snapshot;
    }
    if (loadResult == JsonRollbackLoadResult::Invalid) {
        snapshot.status = StoreReadStatus::Invalid;
        return snapshot;
    }

    bool attemptedSemanticRollback = false;
    while (true) {
        snapshot = StoreSnapshot{};
        const uint8_t version = doc["version"] | 1u;
        snapshot.generation = doc["generation"] | (version == 1 ? 1u : 0u);
        snapshot.legacy = version < STORE_VERSION;
        snapshot.needsRewrite = loadResult == JsonRollbackLoadResult::LoadedRollback;
        snapshot.loadedFromRollback = loadResult == JsonRollbackLoadResult::LoadedRollback;
        if (version > STORE_VERSION || snapshot.generation == 0 || !doc["devices"].is<JsonArray>()) {
            snapshot.status = StoreReadStatus::Invalid;
            return snapshot;
        }
        snapshot.contentCrc = deviceStoreContentCrc(doc);
        if (version >= FIRST_CHECKSUM_STORE_VERSION &&
            (!doc["crc32"].is<uint32_t>() || doc["crc32"].as<uint32_t>() != snapshot.contentCrc)) {
            // The generic loader can detect malformed live JSON, but a
            // syntactically valid file can still fail this store's content
            // checksum. In that exact case, give the same filesystem's
            // committed rollback one semantic validation pass.
            if (!attemptedSemanticRollback && loadResult == JsonRollbackLoadResult::LoadedLive &&
                loadDeviceStoreRollbackDocument(filesystem, MAX_STORE_BYTES, doc)) {
                attemptedSemanticRollback = true;
                loadResult = JsonRollbackLoadResult::LoadedRollback;
                continue;
            }
            snapshot.status = StoreReadStatus::Invalid;
            return snapshot;
        }

        snapshot.status = StoreReadStatus::Valid;
        JsonArray arr = doc["devices"].as<JsonArray>();
        for (JsonObject item : arr) {
            String address = normalizeV1DeviceAddress(String(item["address"] | ""));
            if (address.length() == 0) {
                continue;
            }

            const String name = sanitizeName(String(item["name"] | ""));
            uint8_t defaultProfile = clampDefaultProfileValue(item["defaultProfile"] | 0);
            uint32_t lastSeenMs = item["lastSeenMs"] | 0;
            const V1DetectorSnapshot detectorSnapshot = version >= 3 ? parseSnapshot(item["snapshot"]) :
                                                                      V1DetectorSnapshot{};

            int existing = -1;
            for (size_t i = 0; i < snapshot.devices.size(); ++i) {
                if (snapshot.devices[i].address.equalsIgnoreCase(address)) {
                    existing = static_cast<int>(i);
                    break;
                }
            }

            if (existing >= 0) {
                snapshot.devices[existing].name = name;
                snapshot.devices[existing].defaultProfile = defaultProfile;
                snapshot.devices[existing].lastSeenMs = std::max(snapshot.devices[existing].lastSeenMs, lastSeenMs);
                if (detectorSnapshot.available) snapshot.devices[existing].snapshot = detectorSnapshot;
            } else {
                V1DeviceRecord device;
                device.address = address;
                device.name = name;
                device.defaultProfile = defaultProfile;
                device.lastSeenMs = lastSeenMs;
                device.snapshot = detectorSnapshot;
                snapshot.devices.push_back(device);
            }
        }

        // Existing v2 catalogs already serialize newest first. Retain that durable
        // order: persisted millis() values cannot be compared across boots or wrap.
        if (version == 1) {
            std::sort(snapshot.devices.begin(), snapshot.devices.end(), [](const V1DeviceRecord& lhs,
                                                                          const V1DeviceRecord& rhs) {
                if (lhs.lastSeenMs != rhs.lastSeenMs) return lhs.lastSeenMs > rhs.lastSeenMs;
                return lhs.address < rhs.address;
            });
        }
        if (snapshot.devices.size() > MAX_DEVICES) snapshot.devices.resize(MAX_DEVICES);
        return snapshot;
    }
}

bool V1DeviceStore::loadFromStore() {
    devices_.clear();
    if (!ready_ || !fs_) return false;

    const StoreSnapshot snapshot = readStore(*fs_);
    if (snapshot.status == StoreReadStatus::Missing) return true;
    if (snapshot.status != StoreReadStatus::Valid) return false;

    devices_ = snapshot.devices;
    generation_ = snapshot.generation;
    return true;
}

bool V1DeviceStore::reconcileStores() {
    if (!ready_ || !fs_) return false;

    const StoreSnapshot primary = readStore(*fs_);
    const StoreSnapshot secondary = secondaryFs_ ? readStore(*secondaryFs_) : StoreSnapshot{};
    const bool primaryValid = primary.status == StoreReadStatus::Valid;
    const bool secondaryValid = secondary.status == StoreReadStatus::Valid;

    if (!primaryValid && !secondaryValid) {
        devices_.clear();
        generation_ = 0;
        return primary.status == StoreReadStatus::Missing && secondary.status == StoreReadStatus::Missing;
    }

    bool secondaryWins = secondaryValid && !primaryValid;
    bool generationMustAdvance = false;
    if (primaryValid && secondaryValid) {
        secondaryWins = secondary.generation > primary.generation;
        if (secondary.generation == primary.generation && secondary.contentCrc != primary.contentCrc) {
            // The secondary filesystem is the active store while SD is absent;
            // equal-generation divergence therefore represents an offline edit.
            secondaryWins = true;
            generationMustAdvance = true;
        }
    }

    const StoreSnapshot& winner = secondaryWins ? secondary : primary;
    const StoreSnapshot& loser = secondaryWins ? primary : secondary;
    devices_ = winner.devices;
    generation_ = winner.generation;
    if (loser.status == StoreReadStatus::Invalid && loser.generation >= generation_) {
        generationMustAdvance = true;
    }
    if (generationMustAdvance) {
        generation_ = std::max(primary.generation, secondary.generation) + 1u;
    }

    const bool contentDiffers = !primaryValid || primary.legacy || primary.needsRewrite || generationMustAdvance ||
                                (secondaryFs_ &&
                                 (!secondaryValid || secondary.legacy || secondary.needsRewrite ||
                                  primary.generation != secondary.generation ||
                                  primary.contentCrc != secondary.contentCrc));
    if (!contentDiffers) return true;

    const bool primaryWritten = writeStore(*fs_, generation_, primary.loadedFromRollback);
    const bool secondaryWritten =
        !secondaryFs_ || writeStore(*secondaryFs_, generation_, secondary.loadedFromRollback);
    if (!primaryWritten || !secondaryWritten) {
        Serial.println("[V1Devices] WARN: device-store mirror reconciliation deferred");
    }
    // Keep retry state when either copy could not be repaired. A successful
    // secondary write must not hide a failed primary repair.
    mirrorDirty_ = !primaryWritten || (secondaryFs_ && !secondaryWritten);
    return primaryWritten || secondaryWritten;
}

bool V1DeviceStore::saveToStore() {
    if (!ready_ || !fs_) return false;

    const StoreSnapshot primary = readStore(*fs_);
    const StoreSnapshot secondary = secondaryFs_ ? readStore(*secondaryFs_) : StoreSnapshot{};
    const uint32_t nextGeneration = std::max({generation_, primary.generation, secondary.generation}) + 1u;

    if (!writeStore(*fs_, nextGeneration, primary.loadedFromRollback)) return false;
    generation_ = nextGeneration;
    if (secondaryFs_ && !writeStore(*secondaryFs_, nextGeneration, secondary.loadedFromRollback)) {
        Serial.println("[V1Devices] WARN: secondary device-store mirror deferred");
        mirrorDirty_ = true;
        return false;
    }
    mirrorDirty_ = false;
    return true;
}

bool V1DeviceStore::migrateLegacyFiles(fs::FS* sourceFs) {
    if (!sourceFs) {
        return false;
    }
    if (!sourceFs->exists(LEGACY_ADDR_PATH)) {
        return false;
    }

    std::vector<std::pair<String, String>> names;
    std::vector<std::pair<String, int>> profiles;

    File namesFile = sourceFs->open(LEGACY_NAME_PATH, FILE_READ);
    if (namesFile) {
        while (namesFile.available()) {
            String line = namesFile.readStringUntil('\n');
            line.trim();
            const int sep = line.indexOf('|');
            if (sep <= 0) {
                continue;
            }
            String address = normalizeV1DeviceAddress(line.substring(0, sep));
            if (address.length() == 0) {
                continue;
            }
            String name = sanitizeName(line.substring(sep + 1));
            names.push_back({address, name});
        }
        namesFile.close();
    }

    File profilesFile = sourceFs->open(LEGACY_PROFILE_PATH, FILE_READ);
    if (profilesFile) {
        while (profilesFile.available()) {
            String line = profilesFile.readStringUntil('\n');
            line.trim();
            const int sep = line.indexOf('|');
            if (sep <= 0) {
                continue;
            }
            String address = normalizeV1DeviceAddress(line.substring(0, sep));
            if (address.length() == 0) {
                continue;
            }
            int profile = parseDefaultProfile(line.substring(sep + 1));
            profiles.push_back({address, profile});
        }
        profilesFile.close();
    }

    File addressFile = sourceFs->open(LEGACY_ADDR_PATH, FILE_READ);
    if (!addressFile) {
        return false;
    }

    devices_.clear();

    while (addressFile.available()) {
        String line = addressFile.readStringUntil('\n');
        line.trim();

        String address = normalizeV1DeviceAddress(line);
        if (address.length() == 0) {
            continue;
        }

        if (findDeviceIndex(address) >= 0) {
            continue;
        }

        V1DeviceRecord device;
        device.address = address;
        device.defaultProfile = 0;

        for (const auto& entry : names) {
            if (entry.first.equalsIgnoreCase(address)) {
                device.name = entry.second;
                break;
            }
        }

        for (const auto& entry : profiles) {
            if (entry.first.equalsIgnoreCase(address)) {
                device.defaultProfile = clampDefaultProfileValue(entry.second);
                break;
            }
        }

        devices_.push_back(device);
    }

    addressFile.close();

    if (devices_.empty()) {
        return false;
    }

    // Legacy text has no recency information; retain its deterministic order.
    std::sort(devices_.begin(), devices_.end(), [](const V1DeviceRecord& lhs, const V1DeviceRecord& rhs) {
        return lhs.address < rhs.address;
    });
    trimToCapacity();
    return true;
}

bool V1DeviceStore::begin(fs::FS* filesystem, fs::FS* importFilesystem) {
    fs_ = filesystem;
    secondaryFs_ = importFilesystem && importFilesystem != filesystem ? importFilesystem : nullptr;
    ready_ = fs_ != nullptr;
    dirty_ = false;
    mirrorDirty_ = false;
    generation_ = 0;
    devices_.clear();

    if (!ready_) {
        return false;
    }

    if (!reconcileStores() && !loadFromStore()) {
        devices_.clear();
    }

    if (generation_ == 0) {
        bool migrated = migrateLegacyFiles(fs_);
        if (!migrated && importFilesystem && importFilesystem != fs_) {
            migrated = migrateLegacyFiles(importFilesystem);
        }
        if (migrated) {
            dirty_ = true;
            persistDirtyStore();
        }
    }

    return true;
}

std::vector<V1DeviceRecord> V1DeviceStore::listDevices() const {
    return devices_;
}

bool V1DeviceStore::persistDirtyStore() {
    if (!dirty_ && !mirrorDirty_) {
        return true;
    }
    if (!saveToStore()) {
        return false;
    }
    dirty_ = false;
    return true;
}

bool V1DeviceStore::upsertDeviceInternal(const String& address, bool persistNow) {
    if (!ready_) {
        return false;
    }

    String normalizedAddress = normalizeV1DeviceAddress(address);
    if (normalizedAddress.length() == 0) {
        return false;
    }

    const uint32_t nowMs = millis();
    int index = findDeviceIndex(normalizedAddress);
    if (index >= 0) {
        devices_[index].address = normalizedAddress;
        devices_[index].lastSeenMs = nowMs;
    } else {
        V1DeviceRecord device;
        device.address = normalizedAddress;
        device.lastSeenMs = nowMs;
        devices_.push_back(device);
        index = static_cast<int>(devices_.size()) - 1;
    }

    // The most recent sighting leads the persisted list, even just after boot
    // or millis() rollover. Evict only the least recently seen tail entry.
    std::rotate(devices_.begin(), devices_.begin() + index, devices_.begin() + index + 1);
    trimToCapacity();
    dirty_ = true;
    if (!persistNow) {
        return true;
    }
    return persistDirtyStore();
}

bool V1DeviceStore::upsertDevice(const String& address) {
    return upsertDeviceInternal(address, true);
}

bool V1DeviceStore::bootstrapDevice(const String& address, bool fromDegradedConnection) {
    if (!ready_ || normalizeV1DeviceAddress(address).length() == 0) {
        return false;
    }
    if (!fromDegradedConnection && generation_ != 0) {
        return true;
    }
    return upsertDevice(address);
}

bool V1DeviceStore::touchDeviceInMemory(const String& address) {
    return upsertDeviceInternal(address, false);
}

bool V1DeviceStore::recordSnapshotInMemory(const String& address, const V1DetectorSnapshot& snapshot) {
    if (!snapshot.available || !upsertDeviceInternal(address, false)) return false;
    const String normalizedAddress = normalizeV1DeviceAddress(address);
    const int index = findDeviceIndex(normalizedAddress);
    if (index < 0) return false;
    devices_[index].snapshot = snapshot;
    dirty_ = true;
    return true;
}

bool V1DeviceStore::flushPendingSave() {
    return persistDirtyStore();
}

bool V1DeviceStore::setDeviceName(const String& address, const String& name) {
    if (!ready_) {
        return false;
    }

    String normalizedAddress = normalizeV1DeviceAddress(address);
    if (normalizedAddress.length() == 0) {
        return false;
    }

    String safeName = sanitizeName(name);
    int index = findDeviceIndex(normalizedAddress);
    if (index < 0) {
        V1DeviceRecord device;
        device.address = normalizedAddress;
        device.lastSeenMs = millis();
        devices_.insert(devices_.begin(), device);
        index = 0;
    }

    devices_[index].name = safeName;
    trimToCapacity();
    dirty_ = true;
    return persistDirtyStore();
}

bool V1DeviceStore::setDeviceDefaultProfile(const String& address, uint8_t defaultProfile) {
    if (!ready_) {
        return false;
    }

    String normalizedAddress = normalizeV1DeviceAddress(address);
    if (normalizedAddress.length() == 0) {
        return false;
    }

    int index = findDeviceIndex(normalizedAddress);
    if (index < 0) {
        V1DeviceRecord device;
        device.address = normalizedAddress;
        device.lastSeenMs = millis();
        devices_.insert(devices_.begin(), device);
        index = 0;
    }

    devices_[index].defaultProfile = clampDefaultProfileValue(defaultProfile);
    trimToCapacity();
    dirty_ = true;
    return persistDirtyStore();
}

bool V1DeviceStore::removeDevice(const String& address) {
    if (!ready_) {
        return false;
    }

    String normalizedAddress = normalizeV1DeviceAddress(address);
    if (normalizedAddress.length() == 0) {
        return false;
    }

    const auto it = std::remove_if(devices_.begin(), devices_.end(), [&](const V1DeviceRecord& device) {
        return device.address.equalsIgnoreCase(normalizedAddress);
    });

    if (it == devices_.end()) {
        return true;
    }

    devices_.erase(it, devices_.end());
    dirty_ = true;
    return persistDirtyStore();
}

uint8_t V1DeviceStore::getDeviceDefaultProfile(const String& address) const {
    if (!ready_) {
        return 0;
    }

    String normalizedAddress = normalizeV1DeviceAddress(address);
    if (normalizedAddress.length() == 0) {
        return 0;
    }

    int index = findDeviceIndex(normalizedAddress);
    if (index < 0) {
        return 0;
    }

    return clampDefaultProfileValue(devices_[index].defaultProfile);
}

bool V1DeviceStore::getLatestSnapshot(V1DeviceRecord& device) const {
    if (!ready_) return false;
    for (const V1DeviceRecord& candidate : devices_) {
        if (candidate.snapshot.available) {
            device = candidate;
            return true;
        }
    }
    return false;
}
