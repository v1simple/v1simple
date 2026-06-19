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
constexpr uint8_t STORE_VERSION = 1;

bool isHex(char c) {
    return (c >= '0' && c <= '9') ||
           (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

String clampLen(const String& input, size_t maxLen) {
    if (input.length() <= maxLen) {
        return input;
    }
    return input.substring(0, maxLen);
}

bool copyStoreFile(fs::FS* sourceFs, fs::FS* targetFs) {
    if (!sourceFs || !targetFs || sourceFs == targetFs) {
        return false;
    }
    if (!sourceFs->exists(STORE_PATH)) {
        return false;
    }

    File source = sourceFs->open(STORE_PATH, FILE_READ);
    if (!source) {
        return false;
    }

    if (targetFs->exists(STORE_TMP_PATH)) {
        targetFs->remove(STORE_TMP_PATH);
    }

    File target = targetFs->open(STORE_TMP_PATH, FILE_WRITE);
    if (!target) {
        source.close();
        return false;
    }

    uint8_t buffer[256];
    bool ok = true;
    while (source.available()) {
        size_t readLen = source.read(buffer, sizeof(buffer));
        if (readLen == 0) {
            break;
        }
        if (target.write(buffer, readLen) != readLen) {
            ok = false;
            break;
        }
    }

    target.flush();
    target.close();
    source.close();

    if (!ok) {
        targetFs->remove(STORE_TMP_PATH);
        return false;
    }

    if (!StorageManager::promoteTempFileWithRollback(*targetFs, STORE_TMP_PATH, STORE_PATH)) {
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

}  // namespace

V1DeviceStore v1DeviceStore;

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

void V1DeviceStore::sortAndTrim() {
    std::sort(devices_.begin(), devices_.end(), [](const V1DeviceRecord& lhs, const V1DeviceRecord& rhs) {
        if (lhs.lastSeenMs != rhs.lastSeenMs) {
            return lhs.lastSeenMs > rhs.lastSeenMs;
        }
        return lhs.address < rhs.address;
    });

    if (devices_.size() > MAX_DEVICES) {
        devices_.resize(MAX_DEVICES);
    }
}

bool V1DeviceStore::saveToStore() const {
    if (!ready_ || !fs_) {
        return false;
    }

    JsonDocument doc;
    doc["version"] = STORE_VERSION;
    JsonArray arr = doc["devices"].to<JsonArray>();

    for (const auto& device : devices_) {
        JsonObject obj = arr.add<JsonObject>();
        obj["address"] = device.address;
        obj["name"] = device.name;
        obj["defaultProfile"] = device.defaultProfile;
        obj["lastSeenMs"] = device.lastSeenMs;
    }

    if (fs_->exists(STORE_TMP_PATH)) {
        fs_->remove(STORE_TMP_PATH);
    }

    File file = fs_->open(STORE_TMP_PATH, FILE_WRITE);
    if (!file) {
        return false;
    }

    size_t written = serializeJson(doc, file);
    file.flush();
    file.close();

    if (written == 0) {
        fs_->remove(STORE_TMP_PATH);
        return false;
    }

    if (!StorageManager::promoteTempFileWithRollback(*fs_, STORE_TMP_PATH, STORE_PATH)) {
        return false;
    }

    return true;
}

bool V1DeviceStore::loadFromStore() {
    devices_.clear();

    if (!ready_ || !fs_) {
        return false;
    }

    JsonDocument doc;
    const JsonRollbackLoadResult loadResult =
        loadJsonDocumentWithRollback(*fs_, STORE_PATH, MAX_STORE_BYTES, doc);
    if (loadResult == JsonRollbackLoadResult::Missing) {
        return true;
    }
    if (loadResult == JsonRollbackLoadResult::Invalid) {
        return false;
    }

    if (!doc["devices"].is<JsonArray>()) {
        return true;
    }

    JsonArray arr = doc["devices"].as<JsonArray>();
    for (JsonObject item : arr) {
        String address = normalizeV1DeviceAddress(String(item["address"] | ""));
        if (address.length() == 0) {
            continue;
        }

        const String name = sanitizeName(String(item["name"] | ""));
        uint8_t defaultProfile = clampDefaultProfileValue(item["defaultProfile"] | 0);
        uint32_t lastSeenMs = item["lastSeenMs"] | 0;

        int existing = -1;
        for (size_t i = 0; i < devices_.size(); ++i) {
            if (devices_[i].address.equalsIgnoreCase(address)) {
                existing = static_cast<int>(i);
                break;
            }
        }

        if (existing >= 0) {
            devices_[existing].name = name;
            devices_[existing].defaultProfile = defaultProfile;
            devices_[existing].lastSeenMs = std::max(devices_[existing].lastSeenMs, lastSeenMs);
        } else {
            V1DeviceRecord device;
            device.address = address;
            device.name = name;
            device.defaultProfile = defaultProfile;
            device.lastSeenMs = lastSeenMs;
            devices_.push_back(device);
        }
    }

    sortAndTrim();
    return true;
}

bool V1DeviceStore::migrateStoreFrom(fs::FS* sourceFs) {
    if (!ready_ || !fs_) {
        return false;
    }
    if (fs_->exists(STORE_PATH)) {
        return false;
    }
    return copyStoreFile(sourceFs, fs_);
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

    sortAndTrim();
    return true;
}

bool V1DeviceStore::begin(fs::FS* filesystem, fs::FS* importFilesystem) {
    fs_ = filesystem;
    ready_ = fs_ != nullptr;
    dirty_ = false;
    devices_.clear();

    if (!ready_) {
        return false;
    }

    // Prefer primary store, but migrate prior data from secondary store when needed.
    migrateStoreFrom(importFilesystem);

    if (!loadFromStore()) {
        devices_.clear();
    }

    if (devices_.empty()) {
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
    if (!dirty_) {
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
    }

    sortAndTrim();
    dirty_ = true;
    if (!persistNow) {
        return true;
    }
    return persistDirtyStore();
}

bool V1DeviceStore::upsertDevice(const String& address) {
    return upsertDeviceInternal(address, true);
}

bool V1DeviceStore::touchDeviceInMemory(const String& address) {
    return upsertDeviceInternal(address, false);
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
        devices_.push_back(device);
        index = static_cast<int>(devices_.size()) - 1;
    }

    devices_[index].name = safeName;
    sortAndTrim();
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
        devices_.push_back(device);
        index = static_cast<int>(devices_.size()) - 1;
    }

    devices_[index].defaultProfile = clampDefaultProfileValue(defaultProfile);
    sortAndTrim();
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
