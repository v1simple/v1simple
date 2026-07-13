#pragma once

#include <ArduinoJson.h>
#include <cstdint>

#include "settings.h"
#include "v1_profiles.h"

namespace BackupPayloadBuilder {

enum class BackupTransport : uint8_t {
    HttpDownload = 0,
    SdBackup,
};

struct BuildResult {
    int profilesBackedUp = 0;
};

const char* backupTypeForTransport(BackupTransport transport);
bool isRecognizedBackupType(const char* type);

BuildResult buildBackupDocument(JsonDocument& doc,
                                const V1Settings& settings,
                                const V1ProfileManager& profileManager,
                                BackupTransport transport,
                                uint32_t snapshotMs);

// Returns the CRC32 that would be expected for this document.
// The caller can compare against doc["_crc32"] to detect corruption.
// If doc has no _crc32 key the function still returns the computed value;
// the caller decides whether to treat absence as an error.
uint32_t computeBackupCrc32(const JsonDocument& doc);

}  // namespace BackupPayloadBuilder
