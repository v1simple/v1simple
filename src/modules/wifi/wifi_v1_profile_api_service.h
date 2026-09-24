#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WebServer.h>

#include <cstdint>
#include <vector>

#include "v1_devices.h"
#include "v1_profiles.h"

namespace WifiV1ProfileApiService {

enum class CatalogStatus : uint8_t { Success = 0, NotFound, Busy, IoError, Corrupt, InvalidName };

struct ProfileSummary {
    String name;
    String description;
};

struct Runtime {
    std::vector<String> (*listProfileNames)(void* ctx) = nullptr;
    void* listProfileNamesCtx = nullptr;
    bool (*loadProfileSummary)(const String& name, ProfileSummary& summary, void* ctx) = nullptr;
    void* loadProfileSummaryCtx = nullptr;
    bool (*loadProfileJson)(const String& name, String& json, void* ctx) = nullptr;
    void* loadProfileJsonCtx = nullptr;
    bool (*parseSettingsJson)(const JsonObject& settingsObj, uint8_t outBytes[6], void* ctx) = nullptr;
    void* parseSettingsJsonCtx = nullptr;
    bool (*saveProfile)(const String& name, const String& description,
                        const V1DetectorConfiguration& detector, const uint8_t inBytes[6],
                        bool createOnly, String& error, void* ctx) = nullptr;
    void* saveProfileCtx = nullptr;
    bool (*deleteProfile)(const String& name, void* ctx) = nullptr;
    void* deleteProfileCtx = nullptr;
    bool (*hasCurrentSettings)(void* ctx) = nullptr;
    void* hasCurrentSettingsCtx = nullptr;
    String (*currentSettingsJson)(void* ctx) = nullptr;
    void* currentSettingsJsonCtx = nullptr;
    bool (*v1Connected)(void* ctx) = nullptr;
    void* v1ConnectedCtx = nullptr;
    void (*backupToSd)(void* ctx) = nullptr;
    void* backupToSdCtx = nullptr;
    CatalogStatus (*listProfileNamesResult)(std::vector<String>& names, void* ctx) = nullptr;
    void* listProfileNamesResultCtx = nullptr;
    CatalogStatus (*loadProfileJsonResult)(const String& canonicalName, String& json, void* ctx) = nullptr;
    void* loadProfileJsonResultCtx = nullptr;
    CatalogStatus (*deleteProfileResult)(const String& canonicalName, void* ctx) = nullptr;
    void* deleteProfileResultCtx = nullptr;
    bool (*loadCapturedSnapshot)(V1DeviceRecord& device, void* ctx) = nullptr;
    void* loadCapturedSnapshotCtx = nullptr;
    bool (*capturedSnapshotSourceAvailable)(void* ctx) = nullptr;
    void* capturedSnapshotSourceAvailableCtx = nullptr;
    bool (*settingsJsonForBytes)(const uint8_t bytes[6], String& output, void* ctx) = nullptr;
    void* settingsJsonForBytesCtx = nullptr;
    bool (*profileSchemaReady)(void* ctx) = nullptr;
    void* profileSchemaReadyCtx = nullptr;
    ProfilePageResult (*listProfilePageResult)(const String& after, size_t limit, void* ctx) = nullptr;
    void* listProfilePageResultCtx = nullptr;
    bool (*slotVolumeOverrideConflicts)(const String& profileName,
                                        const V1DetectorConfiguration& detector, void* ctx) = nullptr;
    void* slotVolumeOverrideConflictsCtx = nullptr;
};

void handleApiProfilesListQuery(WebServer& server, const Runtime& runtime,
                                const uint8_t* query, size_t querySize);

void handleApiProfileGetQuery(WebServer& server, const Runtime& runtime,
                              const uint8_t* query, size_t querySize);

void handleApiProfileSave(WebServer& server, const Runtime& runtime, bool (*checkRateLimit)(void* ctx),
                          void* rateLimitCtx);
void handleApiProfileSaveBody(WebServer& server, const Runtime& runtime, const uint8_t* body, size_t bodySize,
                              bool (*checkRateLimit)(void* ctx), void* rateLimitCtx);

void handleApiProfileDelete(WebServer& server, const Runtime& runtime, bool (*checkRateLimit)(void* ctx),
                            void* rateLimitCtx);
void handleApiProfileDeleteBody(WebServer& server, const Runtime& runtime, const uint8_t* body, size_t bodySize,
                                bool (*checkRateLimit)(void* ctx), void* rateLimitCtx);

void handleApiCurrentSettings(WebServer& server, const Runtime& runtime);

} // namespace WifiV1ProfileApiService
