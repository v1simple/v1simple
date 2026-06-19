#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WebServer.h>

#include <cstdint>
#include <vector>

struct V1Settings;

namespace WifiV1ProfileApiService {

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
    bool (*loadProfileSettings)(const String& name, uint8_t outBytes[6], bool& displayOn, void* ctx) = nullptr;
    void* loadProfileSettingsCtx = nullptr;
    bool (*parseSettingsJson)(const JsonObject& settingsObj, uint8_t outBytes[6], void* ctx) = nullptr;
    void* parseSettingsJsonCtx = nullptr;
    bool (*saveProfile)(const String& name,
                        const String& description,
                        bool displayOn,
                        const uint8_t inBytes[6],
                        String& error,
                        void* ctx) = nullptr;
    void* saveProfileCtx = nullptr;
    bool (*deleteProfile)(const String& name, void* ctx) = nullptr;
    void* deleteProfileCtx = nullptr;
    bool (*requestUserBytes)(void* ctx) = nullptr;
    void* requestUserBytesCtx = nullptr;
    bool (*writeUserBytes)(const uint8_t inBytes[6], void* ctx) = nullptr;
    void* writeUserBytesCtx = nullptr;
    const V1Settings& (*getSettings)(void* ctx) = nullptr;
    void* getSettingsCtx = nullptr;
    void (*setDisplayOn)(bool displayOn, void* ctx) = nullptr;
    void* setDisplayOnCtx = nullptr;
    bool (*hasCurrentSettings)(void* ctx) = nullptr;
    void* hasCurrentSettingsCtx = nullptr;
    String (*currentSettingsJson)(void* ctx) = nullptr;
    void* currentSettingsJsonCtx = nullptr;
    bool (*v1Connected)(void* ctx) = nullptr;
    void* v1ConnectedCtx = nullptr;
    void (*backupToSd)(void* ctx) = nullptr;
    void* backupToSdCtx = nullptr;
    bool maintenanceBootActive = false;
};

void handleApiProfilesList(WebServer& server, const Runtime& runtime);

void handleApiProfileGet(WebServer& server, const Runtime& runtime);

void handleApiProfileSave(WebServer& server,
                          const Runtime& runtime,
                          bool (*checkRateLimit)(void* ctx), void* rateLimitCtx);

void handleApiProfileDelete(WebServer& server,
                            const Runtime& runtime,
                            bool (*checkRateLimit)(void* ctx), void* rateLimitCtx);

void handleApiCurrentSettings(WebServer& server, const Runtime& runtime);

void handleApiSettingsPull(WebServer& server,
                           const Runtime& runtime,
                           bool (*checkRateLimit)(void* ctx), void* rateLimitCtx);

void handleApiSettingsPush(WebServer& server,
                           const Runtime& runtime,
                           bool (*checkRateLimit)(void* ctx), void* rateLimitCtx);

}  // namespace WifiV1ProfileApiService
