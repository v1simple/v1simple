#include "v1_devices.h"
#include "json_exact_input.h"
#include "psram_json_document.h"
#include "storage_json_rollback.h"
#include "storage_manager.h"

#include <ArduinoJson.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <limits>
#include <new>

namespace {

constexpr const char* STORE_PATH = "/v1devices.json";
constexpr const char* STORE_TMP_PATH = "/v1devices.tmp";
constexpr const char* LEGACY_ADDR_PATH = "/known_v1.txt";
constexpr const char* LEGACY_NAME_PATH = "/known_v1_names.txt";
constexpr const char* LEGACY_PROFILE_PATH = "/known_v1_profiles.txt";
constexpr uint8_t STORE_VERSION = 4;
constexpr uint8_t FIRST_CHECKSUM_STORE_VERSION = 2;

bool deviceJsonKeyEquals(JsonString actual, const char* expected) {
    const size_t length = std::strlen(expected);
    return actual.size() == length && std::memcmp(actual.c_str(), expected, length) == 0;
}

bool deviceObjectHasExactKeys(JsonObjectConst object, const char* const* required, size_t requiredCount,
                              const char* optional = nullptr) {
    const size_t expected = requiredCount + (optional && !object[optional].isUnbound() ? 1u : 0u);
    if (object.size() != expected) return false;
    for (size_t index = 0; index < requiredCount; ++index)
        if (object[required[index]].isUnbound()) return false;
    for (JsonPairConst pair : object) {
        bool known = optional && deviceJsonKeyEquals(pair.key(), optional);
        for (size_t index = 0; !known && index < requiredCount; ++index) {
            known = deviceJsonKeyEquals(pair.key(), required[index]);
        }
        if (!known) return false;
    }
    return true;
}

bool copyDeviceRecordsChecked(const std::vector<V1DeviceRecord>& source,
                              std::vector<V1DeviceRecord>& destination,
                              size_t reserveCapacity) {
    destination.clear();
    try {
        destination.reserve(reserveCapacity);
        for (const V1DeviceRecord& record : source) {
            V1DeviceRecord copy;
            copy.address = record.address;
            copy.name = record.name;
            if (copy.address.length() != record.address.length() || copy.address != record.address ||
                copy.name.length() != record.name.length() || copy.name != record.name) {
                destination.clear();
                return false;
            }
            copy.defaultProfile = record.defaultProfile;
            copy.lastSeenMs = record.lastSeenMs;
            copy.snapshot = record.snapshot;
            destination.push_back(std::move(copy));
            if (destination.back().address != record.address ||
                destination.back().name != record.name) {
                destination.clear();
                return false;
            }
        }
    } catch (const std::bad_alloc&) {
        destination.clear();
        return false;
    }
    return destination.size() == source.size();
}

bool deviceNameIsCanonical(const String& name, size_t maxLength) {
    if (name.length() > maxLength ||
        !ExactJsonInput::validSemanticString(name.c_str(), name.length())) return false;
    for (size_t index = 0; index < name.length(); ++index) {
        if (static_cast<uint8_t>(name[index]) < 0x20u) return false;
    }
    return name.length() == 0 ||
           (static_cast<uint8_t>(name[0]) > static_cast<uint8_t>(' ') &&
            static_cast<uint8_t>(name[name.length() - 1u]) > static_cast<uint8_t>(' '));
}

bool rawDeviceAddressShapeIsValid(const String& address) {
    size_t begin = 0;
    size_t end = address.length();
    while (begin < end && static_cast<uint8_t>(address[begin]) <= static_cast<uint8_t>(' ')) ++begin;
    while (end > begin && static_cast<uint8_t>(address[end - 1u]) <= static_cast<uint8_t>(' ')) --end;
    if (end - begin != 17u) return false;
    for (size_t index = 0; index < 17u; ++index) {
        const char byte = address[begin + index];
        if ((index + 1u) % 3u == 0u) {
            if (byte != ':' && byte != '-') return false;
        } else if (!((byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f') ||
                     (byte >= 'A' && byte <= 'F'))) {
            return false;
        }
    }
    return true;
}

enum class LegacyLineReadStatus : uint8_t { End = 0, Ok, Invalid, Unavailable };

LegacyLineReadStatus readLegacyLineExact(File& file, String& output) {
    // Legacy records are tiny (17-byte address plus an optional 32-byte name),
    // but tolerate historical padding while keeping ingestion bounded.
    char bytes[256] = {};
    size_t length = 0;
    bool sawByte = false;
    bool overflow = false;
    while (file.available()) {
        const int raw = file.read();
        if (raw < 0) return LegacyLineReadStatus::Unavailable;
        sawByte = true;
        if (raw == '\n') break;
        if (raw == 0) overflow = true;
        if (length + 1u < sizeof(bytes)) {
            bytes[length++] = static_cast<char>(raw);
        } else {
            overflow = true;
        }
    }
    if (!sawByte) return LegacyLineReadStatus::End;
    if (overflow) return LegacyLineReadStatus::Invalid;
    bytes[length] = '\0';
    String parsed(bytes);
    if (parsed.length() != length || (length != 0 && std::memcmp(parsed.c_str(), bytes, length) != 0)) {
        return LegacyLineReadStatus::Unavailable;
    }
    output = std::move(parsed);
    return output.length() == length ? LegacyLineReadStatus::Ok : LegacyLineReadStatus::Unavailable;
}

bool exactSubstring(const String& source, size_t begin, size_t end, String& output) {
    if (begin > end || end > source.length()) return false;
    String parsed = source.substring(begin, end);
    const size_t expected = end - begin;
    if (parsed.length() != expected ||
        (expected != 0 && std::memcmp(parsed.c_str(), source.c_str() + begin, expected) != 0)) return false;
    output = std::move(parsed);
    return output.length() == expected;
}

bool stageLegacyName(const String& source, String& output) {
    if (!ExactJsonInput::validSemanticString(source.c_str(), source.length())) return false;
    for (size_t index = 0; index < source.length(); ++index) {
        if (static_cast<uint8_t>(source[index]) < 0x20u) return false;
    }
    size_t clampedEnd = std::min(source.length(), static_cast<size_t>(32));
    // If the byte limit lands inside a UTF-8 sequence, retain only the last
    // complete code point. The legacy format has no way to signal truncation,
    // but the migrated v4 catalog must remain valid and self-readable.
    if (clampedEnd < source.length()) {
        while (clampedEnd > 0u &&
               (static_cast<uint8_t>(source[clampedEnd]) & 0xc0u) == 0x80u) {
            --clampedEnd;
        }
    }
    size_t begin = 0;
    size_t end = clampedEnd;
    while (begin < end && std::isspace(static_cast<unsigned char>(source[begin])) != 0) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(source[end - 1u])) != 0) --end;
    return exactSubstring(source, begin, end, output) && deviceNameIsCanonical(output, 32u);
}

V1DeviceMutationStatus stageNormalizedDeviceAddress(const String& source, String& normalized) {
    if (!rawDeviceAddressShapeIsValid(source)) return V1DeviceMutationStatus::Invalid;
    normalized = normalizeV1DeviceAddress(source);
    if (normalized.length() != 17u) {
        normalized = String();
        return V1DeviceMutationStatus::Unavailable;
    }
    return V1DeviceMutationStatus::FullyMirrored;
}

bool isHex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

class DeviceStoreCrcWriter {
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

void writeDeviceStoreUnsigned(DeviceStoreCrcWriter& writer, uint32_t value) {
    uint8_t digits[10];
    size_t count = 0;
    do {
        digits[count++] = static_cast<uint8_t>('0' + value % 10u);
        value /= 10u;
    } while (value != 0u);
    while (count > 0) writer.write(digits[--count]);
}

bool deviceStoreContentCrc(const JsonDocument& doc, uint32_t& out) {
    if (doc.overflowed() || !doc.is<JsonObjectConst>() || !doc["devices"].is<JsonArrayConst>()) {
        return false;
    }

    // Preserve the v2-v4 checksum's exact canonical object/key order while
    // streaming directly from the source DOM. This removes both the second
    // ~70 KiB document and the complete serialized String copy.
    DeviceStoreCrcWriter writer;
    static constexpr uint8_t PREFIX[] = {'{', '"', 'v', 'e', 'r', 's', 'i', 'o', 'n', '"', ':'};
    static constexpr uint8_t GENERATION[] = {',', '"', 'g', 'e', 'n', 'e', 'r', 'a', 't', 'i', 'o', 'n', '"', ':'};
    static constexpr uint8_t DEVICES[] = {',', '"', 'd', 'e', 'v', 'i', 'c', 'e', 's', '"', ':'};
    writer.write(PREFIX, sizeof(PREFIX));
    writeDeviceStoreUnsigned(writer, doc["version"] | STORE_VERSION);
    writer.write(GENERATION, sizeof(GENERATION));
    writeDeviceStoreUnsigned(writer, doc["generation"] | 0u);
    writer.write(DEVICES, sizeof(DEVICES));
    serializeJson(doc["devices"].as<JsonArrayConst>(), writer);
    writer.write(static_cast<uint8_t>('}'));
    out = writer.value();
    return true;
}

#ifdef UNIT_TEST
uint32_t deviceStoreContentCrc(const JsonDocument& doc) {
    uint32_t crc = 0;
    return deviceStoreContentCrc(doc, crc) ? crc : 0u;
}
#endif

JsonRollbackLoadResult loadDeviceStoreRollbackDocument(fs::FS& filesystem, size_t maxBytes, JsonDocument& doc) {
    const String rollbackPath = StorageManager::rollbackPathFor(STORE_PATH);
    if (rollbackPath.length() == 0 || !filesystem.exists(rollbackPath.c_str())) return JsonRollbackLoadResult::Missing;

    File file = filesystem.open(rollbackPath.c_str(), FILE_READ);
    if (!file) return JsonRollbackLoadResult::OutOfMemory;
    const size_t fileSize = file.size();
    if (fileSize == 0 || fileSize > maxBytes) {
        file.close();
        return JsonRollbackLoadResult::Invalid;
    }

    PsramJson::Buffer bytes(fileSize);
    if (!bytes || file.read(bytes.data(), fileSize) != fileSize) {
        file.close();
        return JsonRollbackLoadResult::OutOfMemory;
    }
    file.close();
    doc.clear();
    const ExactJsonInput::Status exact = ExactJsonInput::validate(bytes.data(), fileSize);
    if (exact != ExactJsonInput::Status::Ok) {
        doc.clear();
        return exact == ExactJsonInput::Status::MemoryUnavailable ? JsonRollbackLoadResult::OutOfMemory
                                                                  : JsonRollbackLoadResult::Invalid;
    }
    const DeserializationError error = deserializeJson(doc, bytes.data(), fileSize);
    if (error || doc.overflowed()) {
        doc.clear();
        return error == DeserializationError::NoMemory || doc.overflowed()
                   ? JsonRollbackLoadResult::OutOfMemory : JsonRollbackLoadResult::Invalid;
    }
    return JsonRollbackLoadResult::LoadedRollback;
}

bool exactDeviceString(JsonVariantConst value, size_t maxBytes, String& output, bool& unavailable) {
    unavailable = false;
    if (!value.is<const char*>()) return false;
    const JsonString text = value.as<JsonString>();
    if (!text.c_str() || text.size() > maxBytes ||
        !ExactJsonInput::validSemanticString(text.c_str(), text.size())) return false;
    String parsed(text.c_str());
    if (parsed.length() != text.size() ||
        (text.size() != 0 && std::memcmp(parsed.c_str(), text.c_str(), text.size()) != 0)) {
        unavailable = true;
        return false;
    }
    output = std::move(parsed);
    if (output.length() != text.size()) {
        unavailable = true;
        return false;
    }
    return true;
}

bool canonicalStoredAddress(const String& address) {
    if (address.length() != 17) return false;
    for (size_t index = 0; index < 17; ++index) {
        if ((index + 1u) % 3u == 0u) {
            if (address[index] != ':') return false;
        } else if (!((address[index] >= '0' && address[index] <= '9') ||
                     (address[index] >= 'A' && address[index] <= 'F'))) return false;
    }
    return true;
}

int parseDefaultProfile(const String& raw) {
    size_t begin = 0;
    size_t end = raw.length();
    while (begin < end && std::isspace(static_cast<unsigned char>(raw[begin])) != 0) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(raw[end - 1u])) != 0) --end;
    if (begin == end) return 0;
    bool negative = raw[begin] == '-';
    if (negative || raw[begin] == '+') ++begin;
    int value = 0;
    bool sawDigit = false;
    while (begin < end && raw[begin] >= '0' && raw[begin] <= '9') {
        sawDigit = true;
        value = std::min(100, value * 10 + static_cast<int>(raw[begin] - '0'));
        ++begin;
    }
    return sawDigit ? (negative ? -value : value) : 0;
}

bool validSnapshotMode(char mode) {
    return mode == 'A' || mode == 'C' || mode == 'U' ||
           mode == 'l' || mode == 'c' || mode == 'u' || mode == 'L';
}

bool snapshotSemanticsValid(const V1DetectorSnapshot& snapshot) {
    if (!snapshot.available || (snapshot.hasFirmwareVersion && snapshot.firmwareVersion == 0) ||
        (snapshot.hasMode && !validSnapshotMode(snapshot.mode)) ||
        (snapshot.hasCurrentVolume && (snapshot.currentMainVolume > 9 || snapshot.currentMutedVolume > 9)) ||
        (snapshot.hasSavedVolume && (snapshot.savedMainVolume > 9 || snapshot.savedMutedVolume > 9)) ||
        (snapshot.hasBluetoothIndicator &&
         (snapshot.bluetoothIndicator < V1BluetoothIndicatorState::Off ||
          snapshot.bluetoothIndicator > V1BluetoothIndicatorState::On))) return false;
    if (snapshot.hasSweepSections) {
        if (snapshot.sweepSectionCount == 0 || snapshot.sweepSectionCount > snapshot.sweepSections.size()) return false;
        for (uint8_t index = 0; index < snapshot.sweepSectionCount; ++index) {
            const auto& section = snapshot.sweepSections[index];
            const bool unused = section.lowerMHz == 0 && section.upperMHz == 0;
            if (section.index != index || section.count != snapshot.sweepSectionCount ||
                (!unused && (section.lowerMHz == 0 || section.lowerMHz >= section.upperMHz))) return false;
        }
    }
    if (snapshot.hasMaxSweepIndex && snapshot.maxSweepIndex >= snapshot.sweepDefinitions.size()) return false;
    if (snapshot.hasSweepDefinitions) {
        if (!snapshot.hasMaxSweepIndex) return false;
        for (uint8_t index = 0; index <= snapshot.maxSweepIndex; ++index) {
            const auto& definition = snapshot.sweepDefinitions[index];
            const bool unused = definition.lowerMHz == 0 && definition.upperMHz == 0;
            if (definition.index != index ||
                (!unused && (definition.lowerMHz == 0 || definition.lowerMHz >= definition.upperMHz))) return false;
        }
    }
    return true;
}

bool appendSnapshot(JsonObject target, const V1DetectorSnapshot& snapshot) {
    if (!snapshotSemanticsValid(snapshot)) return false;
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
    if (snapshot.hasBluetoothIndicator) {
        target["bluetoothIndicator"] = static_cast<uint8_t>(snapshot.bluetoothIndicator);
    }
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
    bool validSections = snapshot.hasSweepSections && snapshot.sweepSectionCount > 0 &&
                         snapshot.sweepSectionCount <= snapshot.sweepSections.size();
    for (uint8_t index = 0; validSections && index < snapshot.sweepSectionCount; ++index) {
        const auto& section = snapshot.sweepSections[index];
        const bool unused = section.lowerMHz == 0 && section.upperMHz == 0;
        validSections = section.index == index && section.count == snapshot.sweepSectionCount &&
                        (unused || (section.lowerMHz > 0 && section.lowerMHz < section.upperMHz));
    }
    if (snapshot.hasSweepSections && !validSections) return false;
    if (validSections) {
        JsonArray sections = target["sweepSections"].to<JsonArray>();
        for (uint8_t index = 0; index < snapshot.sweepSectionCount; ++index) {
            JsonObject section = sections.add<JsonObject>();
            section["index"] = snapshot.sweepSections[index].index;
            section["count"] = snapshot.sweepSections[index].count;
            section["lowerMHz"] = snapshot.sweepSections[index].lowerMHz;
            section["upperMHz"] = snapshot.sweepSections[index].upperMHz;
        }
    }
    const bool validMax = snapshot.hasMaxSweepIndex && snapshot.maxSweepIndex < snapshot.sweepDefinitions.size();
    if (snapshot.hasMaxSweepIndex && !validMax) return false;
    if (validMax) target["maxSweepIndex"] = snapshot.maxSweepIndex;
    bool validDefinitions = snapshot.hasSweepDefinitions && validMax;
    for (uint8_t index = 0; validDefinitions && index <= snapshot.maxSweepIndex; ++index) {
        const auto& definition = snapshot.sweepDefinitions[index];
        validDefinitions = definition.index == index &&
                           ((definition.lowerMHz == 0 && definition.upperMHz == 0) ||
                            (definition.lowerMHz > 0 && definition.lowerMHz < definition.upperMHz));
    }
    if (snapshot.hasSweepDefinitions && !validDefinitions) return false;
    if (validDefinitions) {
        JsonArray definitions = target["sweepDefinitions"].to<JsonArray>();
        for (uint8_t index = 0; index <= snapshot.maxSweepIndex; ++index) {
            JsonObject definition = definitions.add<JsonObject>();
            definition["index"] = snapshot.sweepDefinitions[index].index;
            definition["lowerMHz"] = snapshot.sweepDefinitions[index].lowerMHz;
            definition["upperMHz"] = snapshot.sweepDefinitions[index].upperMHz;
        }
    }
    return true;
}

bool parseVolumePair(JsonVariantConst source, uint8_t& main, uint8_t& muted) {
    if (!source.is<JsonObjectConst>() || source.size() != 2 || !source["main"].is<int>() ||
        !source["muted"].is<int>()) return false;
    const int parsedMain = source["main"].as<int>();
    const int parsedMuted = source["muted"].as<int>();
    if (parsedMain < 0 || parsedMain > 9 || parsedMuted < 0 || parsedMuted > 9) return false;
    main = static_cast<uint8_t>(parsedMain);
    muted = static_cast<uint8_t>(parsedMuted);
    return true;
}

bool snapshotKeyIsKnown(JsonString key) {
    static constexpr const char* KEYS[] = {
        "capturedBootId", "capturedUptimeMs", "sessionGeneration", "captureTimedOut",
        "firmwareVersion", "userBytes", "mode", "displayOn", "bluetoothIndicator",
        "currentVolume", "savedVolume", "sweepSections", "maxSweepIndex", "sweepDefinitions",
    };
    for (const char* candidate : KEYS) {
        const size_t length = std::strlen(candidate);
        if (key.size() == length && std::memcmp(key.c_str(), candidate, length) == 0) return true;
    }
    return false;
}

// Absence is the one valid representation of "not captured". Once a snapshot
// member is present, every stored claim must be structurally and semantically
// valid; corruption must not be downgraded to a partial or absent capture.
bool parseSnapshot(JsonVariantConst source, V1DetectorSnapshot& snapshot) {
    snapshot = V1DetectorSnapshot{};
    if (source.isUnbound()) return true;
    if (!source.is<JsonObjectConst>() ||
        !source["capturedBootId"].is<uint32_t>() ||
        !source["capturedUptimeMs"].is<uint32_t>() ||
        !source["sessionGeneration"].is<uint32_t>() ||
        !source["captureTimedOut"].is<bool>()) return false;
    for (JsonPairConst pair : source.as<JsonObjectConst>()) {
        if (!snapshotKeyIsKnown(pair.key())) return false;
    }
    snapshot.available = true;
    snapshot.capturedBootId = source["capturedBootId"].as<uint32_t>();
    snapshot.capturedUptimeMs = source["capturedUptimeMs"].as<uint32_t>();
    snapshot.sessionGeneration = source["sessionGeneration"].as<uint32_t>();
    snapshot.captureTimedOut = source["captureTimedOut"].as<bool>();

    if (!source["firmwareVersion"].isUnbound()) {
        if (!source["firmwareVersion"].is<uint32_t>() || source["firmwareVersion"].as<uint32_t>() == 0) return false;
        snapshot.firmwareVersion = source["firmwareVersion"].as<uint32_t>();
        snapshot.hasFirmwareVersion = true;
    }

    if (!source["userBytes"].isUnbound()) {
        if (!source["userBytes"].is<JsonArrayConst>()) return false;
        const JsonArrayConst bytes = source["userBytes"].as<JsonArrayConst>();
        if (bytes.size() != snapshot.userBytes.size()) return false;
        size_t index = 0;
        for (JsonVariantConst value : bytes) {
            if (!value.is<int>() || value.as<int>() < 0 || value.as<int>() > 255) {
                return false;
            }
            snapshot.userBytes[index++] = static_cast<uint8_t>(value.as<int>());
        }
        snapshot.hasUserBytes = true;
    }

    if (!source["mode"].isUnbound()) {
        if (source["mode"].is<uint8_t>()) {
            snapshot.mode = static_cast<char>(source["mode"].as<uint8_t>());
        } else {
        // Accept early development snapshots that encoded the mode as text.
            if (!source["mode"].is<const char*>()) return false;
            const JsonString mode = source["mode"].as<JsonString>();
            if (!mode.c_str() || mode.size() != 1) return false;
            snapshot.mode = mode.c_str()[0];
        }
        if (!validSnapshotMode(snapshot.mode)) return false;
        snapshot.hasMode = true;
    }
    if (!source["displayOn"].isUnbound()) {
        if (!source["displayOn"].is<bool>()) return false;
        snapshot.displayOn = source["displayOn"].as<bool>();
        snapshot.hasDisplayOn = true;
    }
    if (!source["bluetoothIndicator"].isUnbound()) {
        if (!source["bluetoothIndicator"].is<int>()) return false;
        const int value = source["bluetoothIndicator"].as<int>();
        if (value < static_cast<int>(V1BluetoothIndicatorState::Off) ||
            value > static_cast<int>(V1BluetoothIndicatorState::On)) return false;
        snapshot.bluetoothIndicator = static_cast<V1BluetoothIndicatorState>(value);
        snapshot.hasBluetoothIndicator = true;
    }
    if (!source["currentVolume"].isUnbound()) {
        if (!parseVolumePair(source["currentVolume"], snapshot.currentMainVolume,
                             snapshot.currentMutedVolume)) return false;
        snapshot.hasCurrentVolume = true;
    }
    if (!source["savedVolume"].isUnbound()) {
        if (!parseVolumePair(source["savedVolume"], snapshot.savedMainVolume,
                             snapshot.savedMutedVolume)) return false;
        snapshot.hasSavedVolume = true;
    }
    if (!source["sweepSections"].isUnbound()) {
        if (!source["sweepSections"].is<JsonArrayConst>()) return false;
        const JsonArrayConst sections = source["sweepSections"].as<JsonArrayConst>();
        if (sections.size() == 0 || sections.size() > snapshot.sweepSections.size()) return false;
        uint8_t expected = 0;
        for (JsonVariantConst sectionValue : sections) {
            if (!sectionValue.is<JsonObjectConst>()) return false;
            const JsonObjectConst section = sectionValue.as<JsonObjectConst>();
            if (section.size() != 4 || !section["index"].is<int>() || !section["count"].is<int>() ||
                !section["lowerMHz"].is<int>() || !section["upperMHz"].is<int>() ||
                section["index"].as<int>() != expected || section["count"].as<int>() != sections.size() ||
                section["lowerMHz"].as<int>() < 0 || section["lowerMHz"].as<int>() > 65535 ||
                section["upperMHz"].as<int>() < 0 || section["upperMHz"].as<int>() > 65535 ||
                ((section["lowerMHz"].as<int>() == 0) != (section["upperMHz"].as<int>() == 0)) ||
                (section["lowerMHz"].as<int>() != 0 &&
                 section["upperMHz"].as<int>() <= section["lowerMHz"].as<int>())) {
                return false;
            }
            snapshot.sweepSections[expected] = V1SweepSectionObservation{
                expected, static_cast<uint8_t>(sections.size()),
                static_cast<uint16_t>(section["lowerMHz"].as<int>()),
                static_cast<uint16_t>(section["upperMHz"].as<int>())};
            ++expected;
        }
        snapshot.sweepSectionCount = static_cast<uint8_t>(sections.size());
        snapshot.hasSweepSections = true;
    }
    if (!source["maxSweepIndex"].isUnbound()) {
        if (!source["maxSweepIndex"].is<int>() || source["maxSweepIndex"].as<int>() < 0 ||
            source["maxSweepIndex"].as<int>() > 63) return false;
        snapshot.maxSweepIndex = static_cast<uint8_t>(source["maxSweepIndex"].as<int>());
        snapshot.hasMaxSweepIndex = true;
    }
    if (!source["sweepDefinitions"].isUnbound()) {
        if (!snapshot.hasMaxSweepIndex || !source["sweepDefinitions"].is<JsonArrayConst>()) return false;
        const JsonArrayConst definitions = source["sweepDefinitions"].as<JsonArrayConst>();
        if (definitions.size() != static_cast<size_t>(snapshot.maxSweepIndex) + 1u) return false;
        uint8_t expected = 0;
        for (JsonVariantConst definitionValue : definitions) {
            if (!definitionValue.is<JsonObjectConst>()) return false;
            const JsonObjectConst definition = definitionValue.as<JsonObjectConst>();
            if (definition.size() != 3 || !definition["index"].is<int>() ||
                !definition["lowerMHz"].is<int>() || !definition["upperMHz"].is<int>() ||
                definition["index"].as<int>() != expected || definition["lowerMHz"].as<int>() < 0 ||
                definition["upperMHz"].as<int>() < 0 || definition["lowerMHz"].as<int>() > 65535 ||
                definition["upperMHz"].as<int>() > 65535) {
                return false;
            }
            const uint16_t lower = static_cast<uint16_t>(definition["lowerMHz"].as<int>());
            const uint16_t upper = static_cast<uint16_t>(definition["upperMHz"].as<int>());
            if ((lower == 0) != (upper == 0) || (lower != 0 && lower >= upper)) {
                return false;
            }
            snapshot.sweepDefinitions[expected] = V1SweepDefinitionObservation{expected, lower, upper};
            ++expected;
        }
        snapshot.hasSweepDefinitions = true;
    }
    return true;
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

bool V1DeviceStore::writeStore(fs::FS& filesystem, uint32_t generation, bool preserveValidRollback) const {
    if (generation == 0 || devices_.size() > MAX_DEVICES) return false;
    PsramJson::Document doc;
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
            if (!appendSnapshot(obj["snapshot"].to<JsonObject>(), device.snapshot)) return false;
        }
    }

    uint32_t contentCrc = 0;
    if (doc.overflowed() || !deviceStoreContentCrc(doc, contentCrc)) return false;
    doc["crc32"] = contentCrc;

    if (doc.overflowed() || measureJson(doc) > MAX_STORE_BYTES) return false;

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
    PsramJson::Document verified;
    uint32_t verifiedContentCrc = 0;
    PsramJson::Buffer verifyBytes(written);
    const bool bytesAvailable = verifyFile && verifyFile.size() == written && verifyBytes &&
                                verifyFile.read(verifyBytes.data(), written) == written;
    const bool exactCandidate = bytesAvailable &&
                                ExactJsonInput::validate(verifyBytes.data(), written) == ExactJsonInput::Status::Ok;
    const bool validCandidate = exactCandidate && !deserializeJson(verified, verifyBytes.data(), written) &&
                                !verified.overflowed() &&
                                verified["version"].as<uint8_t>() == STORE_VERSION &&
                                verified["generation"].as<uint32_t>() == generation &&
                                verified["crc32"].is<uint32_t>() &&
                                deviceStoreContentCrc(verified, verifiedContentCrc) &&
                                verified["crc32"].as<uint32_t>() == verifiedContentCrc;
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
    PsramJson::Document doc;
    JsonRollbackLoadResult loadResult =
        loadJsonDocumentWithRollback(filesystem, STORE_PATH, MAX_STORE_BYTES, doc);
    StoreSnapshot snapshot;
    if (loadResult == JsonRollbackLoadResult::Missing) {
        return snapshot;
    }
    if (loadResult == JsonRollbackLoadResult::OutOfMemory) {
        storeReadUnavailable_ = true;
        snapshot.status = StoreReadStatus::Unavailable;
        return snapshot;
    }
    if (loadResult == JsonRollbackLoadResult::Invalid) {
        snapshot.status = StoreReadStatus::Invalid;
        return snapshot;
    }

    bool attemptedSemanticRollback = false;
    while (true) {
        snapshot = StoreSnapshot{};
        const auto retryFromSemanticRollback = [&]() {
            if (attemptedSemanticRollback || loadResult != JsonRollbackLoadResult::LoadedLive) return false;
            const JsonRollbackLoadResult rollback =
                loadDeviceStoreRollbackDocument(filesystem, MAX_STORE_BYTES, doc);
            if (rollback == JsonRollbackLoadResult::OutOfMemory) {
                snapshot.status = StoreReadStatus::Unavailable;
                storeReadUnavailable_ = true;
                return false;
            }
            if (rollback != JsonRollbackLoadResult::LoadedRollback) return false;
            attemptedSemanticRollback = true;
            loadResult = rollback;
            return true;
        };
        const JsonVariantConst serializedVersion = doc["version"];
        if (!serializedVersion.isUnbound() &&
            (!serializedVersion.is<uint8_t>() || serializedVersion.as<uint8_t>() == 0 ||
             serializedVersion.as<uint8_t>() > STORE_VERSION)) {
            snapshot.status = StoreReadStatus::Invalid;
            if (retryFromSemanticRollback()) continue;
            return snapshot;
        }
        const uint8_t version = serializedVersion.isUnbound() ? 1u : serializedVersion.as<uint8_t>();
        const JsonVariantConst serializedGeneration = doc["generation"];
        if (!serializedGeneration.isUnbound() &&
            (!serializedGeneration.is<uint32_t>() || serializedGeneration.as<uint32_t>() == 0)) {
            snapshot.status = StoreReadStatus::Invalid;
            if (retryFromSemanticRollback()) continue;
            return snapshot;
        }
        if (version >= FIRST_CHECKSUM_STORE_VERSION && serializedGeneration.isUnbound()) {
            snapshot.status = StoreReadStatus::Invalid;
            if (retryFromSemanticRollback()) continue;
            return snapshot;
        }
        snapshot.generation = serializedGeneration.isUnbound() ? 1u : serializedGeneration.as<uint32_t>();
        snapshot.legacy = version < STORE_VERSION;
        snapshot.needsRewrite = loadResult == JsonRollbackLoadResult::LoadedRollback;
        snapshot.loadedFromRollback = loadResult == JsonRollbackLoadResult::LoadedRollback;
        if (version == STORE_VERSION) {
            static constexpr const char* kRootKeys[] = {"version", "generation", "devices", "crc32"};
            if (!deviceObjectHasExactKeys(doc.as<JsonObjectConst>(), kRootKeys,
                                          sizeof(kRootKeys) / sizeof(kRootKeys[0]))) {
                snapshot.status = StoreReadStatus::Invalid;
                if (retryFromSemanticRollback()) continue;
                return snapshot;
            }
        }
        if (!doc["devices"].is<JsonArray>() ||
            (version < FIRST_CHECKSUM_STORE_VERSION && !doc["crc32"].isUnbound()) ||
            (version == STORE_VERSION && doc["devices"].size() > MAX_DEVICES)) {
            snapshot.status = StoreReadStatus::Invalid;
            if (retryFromSemanticRollback()) continue;
            return snapshot;
        }
        if (!deviceStoreContentCrc(doc, snapshot.contentCrc)) {
            snapshot.status = StoreReadStatus::Unavailable;
            storeReadUnavailable_ = true;
            return snapshot;
        }
        if (version >= FIRST_CHECKSUM_STORE_VERSION &&
            (!doc["crc32"].is<uint32_t>() || doc["crc32"].as<uint32_t>() != snapshot.contentCrc)) {
            // The generic loader can detect malformed live JSON, but a
            // syntactically valid file can still fail this store's content
            // checksum. In that exact case, give the same filesystem's
            // committed rollback one semantic validation pass.
            if (!attemptedSemanticRollback && loadResult == JsonRollbackLoadResult::LoadedLive) {
                const JsonRollbackLoadResult rollback =
                    loadDeviceStoreRollbackDocument(filesystem, MAX_STORE_BYTES, doc);
                if (rollback == JsonRollbackLoadResult::OutOfMemory) {
                    snapshot.status = StoreReadStatus::Unavailable;
                    storeReadUnavailable_ = true;
                    return snapshot;
                }
                if (rollback == JsonRollbackLoadResult::LoadedRollback) {
                    attemptedSemanticRollback = true;
                    loadResult = rollback;
                    continue;
                }
            }
            snapshot.status = StoreReadStatus::Invalid;
            return snapshot;
        }

        snapshot.status = StoreReadStatus::Valid;
        JsonArray arr = doc["devices"].as<JsonArray>();
        try {
            // Current stores are rejected above when over capacity. Legacy
            // stores are reduced deterministically while decoding, so neither
            // path ever retains more than the supported sixteen records.
            snapshot.devices.reserve(MAX_DEVICES);
        } catch (const std::bad_alloc&) {
            snapshot.status = StoreReadStatus::Unavailable;
            storeReadUnavailable_ = true;
            return snapshot;
        }
        bool semanticRecordInvalid = false;
        for (JsonObject item : arr) {
            if (version == STORE_VERSION) {
                static constexpr const char* kItemKeys[] = {"address", "name", "defaultProfile", "lastSeenMs"};
                if (!deviceObjectHasExactKeys(item, kItemKeys, sizeof(kItemKeys) / sizeof(kItemKeys[0]),
                                              "snapshot")) {
                    snapshot.status = StoreReadStatus::Invalid;
                    semanticRecordInvalid = true;
                    break;
                }
            }
            bool unavailable = false;
            String address;
            if (!exactDeviceString(item["address"], 17, address, unavailable)) {
                if (unavailable) {
                    snapshot.status = StoreReadStatus::Unavailable;
                    storeReadUnavailable_ = true;
                    return snapshot;
                }
                snapshot.status = StoreReadStatus::Invalid;
                semanticRecordInvalid = true;
                break;
            }
            if (!canonicalStoredAddress(address)) {
                snapshot.status = StoreReadStatus::Invalid;
                semanticRecordInvalid = true;
                break;
            }
            String name;
            if (!exactDeviceString(item["name"], MAX_NAME_LEN, name, unavailable)) {
                if (unavailable) {
                    snapshot.status = StoreReadStatus::Unavailable;
                    storeReadUnavailable_ = true;
                    return snapshot;
                }
                snapshot.status = StoreReadStatus::Invalid;
                semanticRecordInvalid = true;
                break;
            }
            if (!deviceNameIsCanonical(name, MAX_NAME_LEN)) {
                snapshot.status = StoreReadStatus::Invalid;
                semanticRecordInvalid = true;
                break;
            }
            if (!item["defaultProfile"].is<int>() || item["defaultProfile"].as<int>() < 0 ||
                item["defaultProfile"].as<int>() > 3 || !item["lastSeenMs"].is<uint32_t>()) {
                snapshot.status = StoreReadStatus::Invalid;
                semanticRecordInvalid = true;
                break;
            }
            const uint8_t defaultProfile = static_cast<uint8_t>(item["defaultProfile"].as<int>());
            const uint32_t lastSeenMs = item["lastSeenMs"].as<uint32_t>();
            V1DetectorSnapshot detectorSnapshot;
            if (version >= 3 && !parseSnapshot(item["snapshot"], detectorSnapshot)) {
                snapshot.status = StoreReadStatus::Invalid;
                semanticRecordInvalid = true;
                break;
            }

            int existing = -1;
            for (size_t i = 0; i < snapshot.devices.size(); ++i) {
                if (snapshot.devices[i].address.equalsIgnoreCase(address)) {
                    existing = static_cast<int>(i);
                    break;
                }
            }

            if (existing >= 0) {
                if (version == STORE_VERSION) {
                    snapshot.status = StoreReadStatus::Invalid;
                    semanticRecordInvalid = true;
                    break;
                }
                snapshot.devices[existing].name = std::move(name);
                snapshot.devices[existing].defaultProfile = defaultProfile;
                snapshot.devices[existing].lastSeenMs = std::max(snapshot.devices[existing].lastSeenMs, lastSeenMs);
                if (detectorSnapshot.available) snapshot.devices[existing].snapshot = detectorSnapshot;
            } else {
                V1DeviceRecord device;
                device.address = std::move(address);
                device.name = std::move(name);
                device.defaultProfile = defaultProfile;
                device.lastSeenMs = lastSeenMs;
                device.snapshot = detectorSnapshot;
                if (snapshot.devices.size() < MAX_DEVICES) {
                    snapshot.devices.push_back(std::move(device));
                } else if (version == 1) {
                    auto worst = std::min_element(
                        snapshot.devices.begin(), snapshot.devices.end(),
                        [](const V1DeviceRecord& lhs, const V1DeviceRecord& rhs) {
                            if (lhs.lastSeenMs != rhs.lastSeenMs) return lhs.lastSeenMs < rhs.lastSeenMs;
                            return lhs.address > rhs.address;
                        });
                    const bool candidatePrecedesWorst =
                        device.lastSeenMs > worst->lastSeenMs ||
                        (device.lastSeenMs == worst->lastSeenMs && device.address < worst->address);
                    if (candidatePrecedesWorst) *worst = std::move(device);
                }
            }
        }

        if (semanticRecordInvalid) {
            if (retryFromSemanticRollback()) continue;
            return snapshot;
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
        return snapshot;
    }
}

bool V1DeviceStore::loadFromStore() {
    devices_.clear();
    if (!ready_ || !fs_) return false;

    StoreSnapshot snapshot = readStore(*fs_);
    if (snapshot.status == StoreReadStatus::Missing) {
        catalogStatus_ = StoreReadStatus::Missing;
        return true;
    }
    if (snapshot.status != StoreReadStatus::Valid) {
        catalogStatus_ = snapshot.status;
        return false;
    }

    devices_ = std::move(snapshot.devices);
    generation_ = snapshot.generation;
    catalogStatus_ = StoreReadStatus::Valid;
    return true;
}

bool V1DeviceStore::reconcileStores() {
    if (!ready_ || !fs_) return false;

    storeReadUnavailable_ = false;
    StoreSnapshot primary = readStore(*fs_);
    StoreSnapshot secondary = secondaryFs_ ? readStore(*secondaryFs_) : StoreSnapshot{};
    if (storeReadUnavailable_) {
        catalogStatus_ = StoreReadStatus::Unavailable;
        return false;
    }
    const bool primaryValid = primary.status == StoreReadStatus::Valid;
    const bool secondaryValid = secondary.status == StoreReadStatus::Valid;

    if (!primaryValid && !secondaryValid) {
        devices_.clear();
        generation_ = 0;
        const bool bothMissing = primary.status == StoreReadStatus::Missing &&
                                 secondary.status == StoreReadStatus::Missing;
        catalogStatus_ = bothMissing ? StoreReadStatus::Missing : StoreReadStatus::Invalid;
        return bothMissing;
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

    const StoreSnapshot& loser = secondaryWins ? primary : secondary;
    generation_ = secondaryWins ? secondary.generation : primary.generation;
    devices_ = secondaryWins ? std::move(secondary.devices) : std::move(primary.devices);
    catalogStatus_ = StoreReadStatus::Valid;
    if (loser.status == StoreReadStatus::Invalid && loser.generation >= generation_) {
        generationMustAdvance = true;
    }
    if (generationMustAdvance) {
        const uint32_t maximumGeneration = std::max(primary.generation, secondary.generation);
        if (maximumGeneration == std::numeric_limits<uint32_t>::max()) {
            mirrorDirty_ = true;
            storeReadUnavailable_ = true;
            catalogStatus_ = StoreReadStatus::Unavailable;
            return false;
        }
        generation_ = maximumGeneration + 1u;
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

V1DeviceMutationResult V1DeviceStore::saveToStoreResult() {
    if (!ready_ || !fs_ || !catalogReadable()) {
        return {V1DeviceMutationStatus::Unavailable};
    }

    storeReadUnavailable_ = false;
    const StoreSnapshot primary = readStore(*fs_);
    const StoreSnapshot secondary = secondaryFs_ ? readStore(*secondaryFs_) : StoreSnapshot{};
    if (storeReadUnavailable_) {
        // This save started only from an already authoritative in-memory
        // catalog. A transient verification allocation failure must defer the
        // write without converting that validated origin into the boot-time
        // "unknown catalog" state; the next flush re-reads both durable copies
        // before promotion. Boot/reconciliation OOM still sets Unavailable.
        return {V1DeviceMutationStatus::Unavailable};
    }
    const uint32_t maximumGeneration = std::max({generation_, primary.generation, secondary.generation});
    if (maximumGeneration == std::numeric_limits<uint32_t>::max()) {
        return {V1DeviceMutationStatus::NotCommitted};
    }
    const uint32_t nextGeneration = maximumGeneration + 1u;

    if (!writeStore(*fs_, nextGeneration, primary.loadedFromRollback)) {
        return {V1DeviceMutationStatus::NotCommitted};
    }
    generation_ = nextGeneration;
    dirty_ = false;
    catalogStatus_ = StoreReadStatus::Valid;
    if (secondaryFs_ && !writeStore(*secondaryFs_, nextGeneration, secondary.loadedFromRollback)) {
        Serial.println("[V1Devices] WARN: secondary device-store mirror deferred");
        mirrorDirty_ = true;
        return {V1DeviceMutationStatus::PrimaryCommittedMirrorPending};
    }
    mirrorDirty_ = false;
    return {V1DeviceMutationStatus::FullyMirrored};
}

bool V1DeviceStore::saveToStore() {
    return saveToStoreResult().fullyMirrored();
}

bool V1DeviceStore::migrateLegacyFiles(fs::FS* sourceFs) {
    if (!sourceFs) {
        return false;
    }
    if (!sourceFs->exists(LEGACY_ADDR_PATH)) {
        return false;
    }

    std::vector<V1DeviceRecord> candidate;
    try {
        candidate.reserve(MAX_DEVICES);
    } catch (const std::bad_alloc&) {
        return false;
    }

    File addressFile = sourceFs->open(LEGACY_ADDR_PATH, FILE_READ);
    if (!addressFile) {
        return false;
    }

    while (addressFile.available()) {
        String line;
        const LegacyLineReadStatus lineStatus = readLegacyLineExact(addressFile, line);
        if (lineStatus == LegacyLineReadStatus::End) break;
        if (lineStatus != LegacyLineReadStatus::Ok) {
            addressFile.close();
            return false;
        }
        line.trim();

        String address;
        const V1DeviceMutationStatus addressStatus = stageNormalizedDeviceAddress(line, address);
        if (addressStatus == V1DeviceMutationStatus::Invalid) continue;
        if (addressStatus != V1DeviceMutationStatus::FullyMirrored) {
            addressFile.close();
            return false;
        }

        const auto existing = std::find_if(candidate.begin(), candidate.end(), [&](const V1DeviceRecord& record) {
            return record.address.equalsIgnoreCase(address);
        });
        if (existing != candidate.end()) {
            continue;
        }

        V1DeviceRecord device;
        device.address = std::move(address);
        if (device.address.length() != 17u) {
            addressFile.close();
            return false;
        }
        device.defaultProfile = 0;

        if (candidate.size() < MAX_DEVICES) {
            try {
                candidate.push_back(std::move(device));
            } catch (const std::bad_alloc&) {
                addressFile.close();
                return false;
            }
        } else {
            auto largest = std::max_element(candidate.begin(), candidate.end(),
                                            [](const V1DeviceRecord& lhs, const V1DeviceRecord& rhs) {
                                                return lhs.address < rhs.address;
                                            });
            if (device.address < largest->address) *largest = std::move(device);
        }
    }

    addressFile.close();

    if (candidate.empty()) {
        return false;
    }

    const auto applyLegacyMap = [&](const char* path, bool names) {
        File mapFile = sourceFs->open(path, FILE_READ);
        if (!mapFile) return true;
        while (mapFile.available()) {
            String line;
            const LegacyLineReadStatus lineStatus = readLegacyLineExact(mapFile, line);
            if (lineStatus == LegacyLineReadStatus::End) break;
            if (lineStatus != LegacyLineReadStatus::Ok) {
                mapFile.close();
                return false;
            }
            line.trim();
            const int separator = line.indexOf('|');
            if (separator <= 0) continue;
            String rawAddress;
            if (!exactSubstring(line, 0, static_cast<size_t>(separator), rawAddress)) {
                mapFile.close();
                return false;
            }
            String address;
            const V1DeviceMutationStatus addressStatus = stageNormalizedDeviceAddress(rawAddress, address);
            if (addressStatus == V1DeviceMutationStatus::Invalid) continue;
            if (addressStatus != V1DeviceMutationStatus::FullyMirrored) {
                mapFile.close();
                return false;
            }
            auto target = std::find_if(candidate.begin(), candidate.end(), [&](const V1DeviceRecord& record) {
                return record.address == address;
            });
            if (target == candidate.end()) continue;
            String value;
            if (!exactSubstring(line, static_cast<size_t>(separator) + 1u, line.length(), value)) {
                mapFile.close();
                return false;
            }
            if (names) {
                String prepared;
                if (!stageLegacyName(value, prepared)) {
                    mapFile.close();
                    return false;
                }
                target->name = std::move(prepared);
            } else {
                target->defaultProfile = clampDefaultProfileValue(parseDefaultProfile(value));
            }
        }
        mapFile.close();
        return true;
    };
    if (!applyLegacyMap(LEGACY_NAME_PATH, true) || !applyLegacyMap(LEGACY_PROFILE_PATH, false)) return false;

    // Legacy text has no recency information; retain its deterministic order.
    std::sort(candidate.begin(), candidate.end(), [](const V1DeviceRecord& lhs, const V1DeviceRecord& rhs) {
        return lhs.address < rhs.address;
    });
    devices_.swap(candidate);
    return true;
}

bool V1DeviceStore::begin(fs::FS* filesystem, fs::FS* importFilesystem) {
    fs_ = filesystem;
    secondaryFs_ = importFilesystem && importFilesystem != filesystem ? importFilesystem : nullptr;
    ready_ = fs_ != nullptr;
    dirty_ = false;
    mirrorDirty_ = false;
    generation_ = 0;
    storeReadUnavailable_ = false;
    catalogStatus_ = StoreReadStatus::Missing;
    devices_.clear();

    if (!ready_) {
        return false;
    }

    if (!reconcileStores() && !storeReadUnavailable_ && !loadFromStore()) {
        devices_.clear();
    }

    if (generation_ == 0 && !storeReadUnavailable_) {
        bool migrated = migrateLegacyFiles(fs_);
        if (!migrated && importFilesystem && importFilesystem != fs_) {
            migrated = migrateLegacyFiles(importFilesystem);
        }
        if (migrated) {
            // The fully staged legacy candidate is now the only validated
            // source. Permit it to replace a malformed current file, but do
            // not expose it unless the authoritative promotion commits.
            catalogStatus_ = StoreReadStatus::Missing;
            dirty_ = true;
            const V1DeviceMutationResult persisted = saveToStoreResult();
            if (persisted.committed()) {
                dirty_ = false;
            } else {
                // Never expose a migrated catalog that could not be made
                // authoritative. The legacy files remain available for a
                // later boot retry.
                devices_.clear();
                generation_ = 0;
                dirty_ = false;
                mirrorDirty_ = false;
                storeReadUnavailable_ = true;
                catalogStatus_ = StoreReadStatus::Unavailable;
            }
        }
    }

    return true;
}

std::vector<V1DeviceRecord> V1DeviceStore::listDevices() const {
    return devices_;
}

bool V1DeviceStore::catalogReadable() const {
    return catalogStatus_ == StoreReadStatus::Missing || catalogStatus_ == StoreReadStatus::Valid;
}

bool V1DeviceStore::containsDeviceChecked(const String& address, bool& present) const {
    present = false;
    if (!ready_ || !catalogReadable()) return false;
    String normalized;
    if (stageNormalizedDeviceAddress(address, normalized) != V1DeviceMutationStatus::FullyMirrored) return false;
    present = findDeviceIndex(normalized) >= 0;
    return true;
}

bool V1DeviceStore::listDevicesChecked(std::vector<V1DeviceRecord>& output) const {
    if (!catalogReadable()) return false;
    return copyDeviceRecordsChecked(devices_, output, devices_.size());
}

bool V1DeviceStore::persistDirtyStore() {
    if (!catalogReadable()) return false;
    if (!dirty_ && !mirrorDirty_) {
        return true;
    }
    if (!saveToStore()) {
        return false;
    }
    dirty_ = false;
    return true;
}

bool V1DeviceStore::buildUpsertCandidate(const String& address, const V1DetectorSnapshot* snapshot,
                                         std::vector<V1DeviceRecord>& candidate) const {
    String normalizedAddress;
    if (stageNormalizedDeviceAddress(address, normalizedAddress) != V1DeviceMutationStatus::FullyMirrored ||
        !copyDeviceRecordsChecked(devices_, candidate, MAX_DEVICES)) return false;
    const uint32_t nowMs = millis();
    int index = -1;
    for (size_t position = 0; position < candidate.size(); ++position) {
        if (candidate[position].address.equalsIgnoreCase(normalizedAddress)) {
            index = static_cast<int>(position);
            break;
        }
    }
    if (index >= 0) {
        candidate[static_cast<size_t>(index)].lastSeenMs = nowMs;
        if (snapshot) candidate[static_cast<size_t>(index)].snapshot = *snapshot;
    } else {
        V1DeviceRecord device;
        device.address = std::move(normalizedAddress);
        if (device.address.length() != 17u) return false;
        device.lastSeenMs = nowMs;
        if (snapshot) device.snapshot = *snapshot;
        if (candidate.size() == MAX_DEVICES) candidate.pop_back();
        try {
            candidate.push_back(std::move(device));
        } catch (const std::bad_alloc&) {
            return false;
        }
        index = static_cast<int>(candidate.size()) - 1;
    }

    // The most recent sighting leads the persisted list, even just after boot
    // or millis() rollover. Evict only the least recently seen tail entry.
    std::rotate(candidate.begin(), candidate.begin() + index, candidate.begin() + index + 1);
    return true;
}

bool V1DeviceStore::upsertDeviceInternal(const String& address, bool persistNow) {
    if (!ready_ || !catalogReadable()) return false;
    std::vector<V1DeviceRecord> candidate;
    if (!buildUpsertCandidate(address, nullptr, candidate)) return false;

    const uint32_t generationBefore = generation_;
    const bool dirtyBefore = dirty_;
    const bool mirrorDirtyBefore = mirrorDirty_;
    devices_.swap(candidate);
    dirty_ = true;
    if (!persistNow) {
        return true;
    }
    const V1DeviceMutationResult result = saveToStoreResult();
    if (!result.committed()) {
        devices_.swap(candidate);
        generation_ = generationBefore;
        dirty_ = dirtyBefore;
        mirrorDirty_ = mirrorDirtyBefore;
    }
    return result.committed();
}

bool V1DeviceStore::upsertDevice(const String& address) {
    return upsertDeviceInternal(address, true);
}

bool V1DeviceStore::bootstrapDevice(const String& address, bool fromDegradedConnection) {
    if (!ready_ || !catalogReadable() || normalizeV1DeviceAddress(address).length() == 0) {
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
    if (!snapshotSemanticsValid(snapshot) || !ready_ || !catalogReadable()) return false;
    std::vector<V1DeviceRecord> candidate;
    if (!buildUpsertCandidate(address, &snapshot, candidate)) return false;
    devices_.swap(candidate);
    dirty_ = true;
    return true;
}

bool V1DeviceStore::flushPendingSave() {
    return persistDirtyStore();
}

V1DeviceMutationResult V1DeviceStore::setDeviceName(const String& address, const String& name) {
    if (!ready_ || !catalogReadable()) {
        return {V1DeviceMutationStatus::Unavailable};
    }
    if (!deviceNameIsCanonical(name, MAX_NAME_LEN)) {
        return {V1DeviceMutationStatus::Invalid};
    }

    String normalizedAddress;
    const V1DeviceMutationStatus addressStatus =
        stageNormalizedDeviceAddress(address, normalizedAddress);
    if (addressStatus != V1DeviceMutationStatus::FullyMirrored) {
        return {addressStatus};
    }

    String safeName = name;
    if (safeName.length() != name.length() || safeName != name) {
        return {V1DeviceMutationStatus::Unavailable};
    }
    std::vector<V1DeviceRecord> candidate;
    if (!copyDeviceRecordsChecked(devices_, candidate, MAX_DEVICES)) {
        return {V1DeviceMutationStatus::Unavailable};
    }
    int index = -1;
    for (size_t position = 0; position < candidate.size(); ++position) {
        if (candidate[position].address.equalsIgnoreCase(normalizedAddress)) {
            index = static_cast<int>(position);
            break;
        }
    }
    if (index < 0) {
        V1DeviceRecord device;
        device.address = std::move(normalizedAddress);
        if (device.address.length() != 17u) {
            return {V1DeviceMutationStatus::Unavailable};
        }
        device.name = std::move(safeName);
        device.lastSeenMs = millis();
        if (candidate.size() == MAX_DEVICES) candidate.pop_back();
        try {
            candidate.insert(candidate.begin(), std::move(device));
        } catch (const std::bad_alloc&) {
            return {V1DeviceMutationStatus::Unavailable};
        }
    } else {
        candidate[static_cast<size_t>(index)].name = std::move(safeName);
    }

    const uint32_t generationBefore = generation_;
    const bool dirtyBefore = dirty_;
    const bool mirrorDirtyBefore = mirrorDirty_;
    devices_.swap(candidate);
    dirty_ = true;
    const V1DeviceMutationResult result = saveToStoreResult();
    if (!result.committed()) {
        devices_.swap(candidate);
        generation_ = generationBefore;
        dirty_ = dirtyBefore;
        mirrorDirty_ = mirrorDirtyBefore;
    }
    return result;
}

V1DeviceMutationResult V1DeviceStore::setDeviceDefaultProfile(const String& address,
                                                               uint8_t defaultProfile) {
    if (!ready_ || !catalogReadable()) {
        return {V1DeviceMutationStatus::Unavailable};
    }
    if (defaultProfile > 3) return {V1DeviceMutationStatus::Invalid};

    String normalizedAddress;
    const V1DeviceMutationStatus addressStatus =
        stageNormalizedDeviceAddress(address, normalizedAddress);
    if (addressStatus != V1DeviceMutationStatus::FullyMirrored) {
        return {addressStatus};
    }

    std::vector<V1DeviceRecord> candidate;
    if (!copyDeviceRecordsChecked(devices_, candidate, MAX_DEVICES)) {
        return {V1DeviceMutationStatus::Unavailable};
    }
    int index = -1;
    for (size_t position = 0; position < candidate.size(); ++position) {
        if (candidate[position].address.equalsIgnoreCase(normalizedAddress)) {
            index = static_cast<int>(position);
            break;
        }
    }
    if (index < 0) {
        V1DeviceRecord device;
        device.address = std::move(normalizedAddress);
        if (device.address.length() != 17u) {
            return {V1DeviceMutationStatus::Unavailable};
        }
        device.defaultProfile = defaultProfile;
        device.lastSeenMs = millis();
        if (candidate.size() == MAX_DEVICES) candidate.pop_back();
        try {
            candidate.insert(candidate.begin(), std::move(device));
        } catch (const std::bad_alloc&) {
            return {V1DeviceMutationStatus::Unavailable};
        }
    } else {
        candidate[static_cast<size_t>(index)].defaultProfile = defaultProfile;
    }

    const uint32_t generationBefore = generation_;
    const bool dirtyBefore = dirty_;
    const bool mirrorDirtyBefore = mirrorDirty_;
    devices_.swap(candidate);
    dirty_ = true;
    const V1DeviceMutationResult result = saveToStoreResult();
    if (!result.committed()) {
        devices_.swap(candidate);
        generation_ = generationBefore;
        dirty_ = dirtyBefore;
        mirrorDirty_ = mirrorDirtyBefore;
    }
    return result;
}

V1DeviceMutationResult V1DeviceStore::removeDevice(const String& address) {
    if (!ready_ || !catalogReadable()) {
        return {V1DeviceMutationStatus::Unavailable};
    }

    String normalizedAddress;
    const V1DeviceMutationStatus addressStatus =
        stageNormalizedDeviceAddress(address, normalizedAddress);
    if (addressStatus != V1DeviceMutationStatus::FullyMirrored) {
        return {addressStatus};
    }

    std::vector<V1DeviceRecord> candidate;
    if (!copyDeviceRecordsChecked(devices_, candidate, MAX_DEVICES)) {
        return {V1DeviceMutationStatus::Unavailable};
    }
    const auto it = std::remove_if(candidate.begin(), candidate.end(), [&](const V1DeviceRecord& device) {
        return device.address.equalsIgnoreCase(normalizedAddress);
    });

    if (it == candidate.end()) {
        return !dirty_ && !mirrorDirty_
                   ? V1DeviceMutationResult{V1DeviceMutationStatus::FullyMirrored}
                   : saveToStoreResult();
    }

    candidate.erase(it, candidate.end());
    const uint32_t generationBefore = generation_;
    const bool dirtyBefore = dirty_;
    const bool mirrorDirtyBefore = mirrorDirty_;
    devices_.swap(candidate);
    dirty_ = true;
    const V1DeviceMutationResult result = saveToStoreResult();
    if (!result.committed()) {
        devices_.swap(candidate);
        generation_ = generationBefore;
        dirty_ = dirtyBefore;
        mirrorDirty_ = mirrorDirtyBefore;
    }
    return result;
}

V1DeviceDefaultProfileResult
V1DeviceStore::getDeviceDefaultProfileChecked(const String& address) const {
    if (!ready_ || !catalogReadable()) {
        return {V1DeviceDefaultProfileStatus::Unavailable, 0};
    }

    String normalizedAddress;
    if (stageNormalizedDeviceAddress(address, normalizedAddress) !=
        V1DeviceMutationStatus::FullyMirrored) {
        return {V1DeviceDefaultProfileStatus::Unavailable, 0};
    }

    int index = findDeviceIndex(normalizedAddress);
    if (index < 0) {
        return {V1DeviceDefaultProfileStatus::NoOverride, 0};
    }

    const uint8_t profile = clampDefaultProfileValue(devices_[index].defaultProfile);
    return profile == 0
               ? V1DeviceDefaultProfileResult{V1DeviceDefaultProfileStatus::NoOverride, 0}
               : V1DeviceDefaultProfileResult{V1DeviceDefaultProfileStatus::Found, profile};
}

uint8_t V1DeviceStore::getDeviceDefaultProfile(const String& address) const {
    const V1DeviceDefaultProfileResult result = getDeviceDefaultProfileChecked(address);
    return result.status == V1DeviceDefaultProfileStatus::Found ? result.profile : 0;
}

bool V1DeviceStore::getLatestSnapshot(V1DeviceRecord& device) const {
    if (!ready_ || !catalogReadable()) return false;
    for (const V1DeviceRecord& candidate : devices_) {
        if (candidate.snapshot.available) {
            V1DeviceRecord copy;
            copy.address = candidate.address;
            copy.name = candidate.name;
            if (copy.address.length() != candidate.address.length() || copy.address != candidate.address ||
                copy.name.length() != candidate.name.length() || copy.name != candidate.name) return false;
            copy.defaultProfile = candidate.defaultProfile;
            copy.lastSeenMs = candidate.lastSeenMs;
            copy.snapshot = candidate.snapshot;
            device = std::move(copy);
            return device.address == candidate.address && device.name == candidate.name;
        }
    }
    return false;
}

V1DeviceSnapshotStatus V1DeviceStore::getSnapshotForAddressChecked(
    const String& address, V1DeviceRecord& device) const {
    if (!ready_ || !catalogReadable()) return V1DeviceSnapshotStatus::Unavailable;
    const String normalized = normalizeV1DeviceAddress(address);
    if (normalized.length() != 17u || normalized != address) {
        return V1DeviceSnapshotStatus::NotFound;
    }
    const int index = findDeviceIndex(normalized);
    if (index < 0 || !devices_[static_cast<size_t>(index)].snapshot.available) {
        return V1DeviceSnapshotStatus::NotFound;
    }
    try {
        device = devices_[static_cast<size_t>(index)];
    } catch (const std::bad_alloc&) {
        return V1DeviceSnapshotStatus::Unavailable;
    }
    return device.address == normalized && device.snapshot.available
               ? V1DeviceSnapshotStatus::Found
               : V1DeviceSnapshotStatus::Unavailable;
}
