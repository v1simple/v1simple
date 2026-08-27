#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WebServer.h>

#include <cstdint>
#include <vector>

namespace WifiV1ProfileApiService {

enum class CatalogStatus : uint8_t { Success = 0, NotFound, Busy, IoError, Corrupt, InvalidName };

struct ProfileSummary {
    String name;
    String description;
    bool displayOn = true;
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
    bool (*saveProfile)(const String& name, const String& description, bool displayOn, const uint8_t inBytes[6],
                        String& error, void* ctx) = nullptr;
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
};

void handleApiProfilesList(WebServer& server, const Runtime& runtime);

void handleApiProfileGet(WebServer& server, const Runtime& runtime);

void handleApiProfileSave(WebServer& server, const Runtime& runtime, bool (*checkRateLimit)(void* ctx),
                          void* rateLimitCtx);

void handleApiProfileDelete(WebServer& server, const Runtime& runtime, bool (*checkRateLimit)(void* ctx),
                            void* rateLimitCtx);

void handleApiCurrentSettings(WebServer& server, const Runtime& runtime);

} // namespace WifiV1ProfileApiService
