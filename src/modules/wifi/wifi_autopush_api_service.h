#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WebServer.h>

#include <cstdint>

#include "../../v1_settings_operation.h"

namespace WifiAutoPushApiService {

enum class ProfileAssignmentStatus : uint8_t {
    Success = 0,
    NotFound,
    Busy,
    IoError,
    Corrupt,
    InvalidName,
};

enum class OperationTargetStatus : uint8_t {
    Allowed = 0,
    NotFound,
    UnsupportedFirmware,
    Unavailable,
};

struct SlotConfig {
    String name;
    String profile;
    int mode = 0;
    uint16_t color = 0;
    uint8_t volume = 0;
    uint8_t muteVolume = 0;
    bool volumeConfigured = false;
    bool darkMode = false;
    bool darkModeConfigured = false;
    bool muteToZero = false;
    uint8_t alertPersist = 0;
    bool priorityArrowOnly = false;
};

struct SlotsSnapshot {
    bool enabled = false;
    bool profileOwned = false;
    int activeSlot = 0;
    SlotConfig slots[3];
};

struct SlotUpdateRequest {
    int slot = 0;
    bool profileOwned = false;
    bool hasName = false;
    String name;
    bool hasColor = false;
    uint16_t color = 0;
    bool hasVolume = false;
    uint8_t volume = 0;
    bool hasMuteVolume = false;
    uint8_t muteVolume = 0;
    bool hasVolumeConfigured = false;
    bool volumeConfigured = false;
    bool hasDarkMode = false;
    bool darkMode = false;
    bool hasDarkModeConfigured = false;
    bool darkModeConfigured = false;
    bool hasMuteToZero = false;
    bool muteToZero = false;
    bool hasAlertPersist = false;
    uint8_t alertPersist = 0;
    bool hasPriorityArrowOnly = false;
    bool priorityArrowOnly = false;
    String profile;
    int mode = 0;
};

struct ActivationRequest {
    int slot = 0;
    bool enable = true;
};

struct OperationStartRequest {
    V1SettingsOperationStore::Kind kind = V1SettingsOperationStore::Kind::None;
    int slot = -1;
    String profileName;
    String targetAddress;
};

struct Runtime {
    void (*loadSlotsSnapshot)(SlotsSnapshot& snapshot, void* ctx) = nullptr;
    void* loadSlotsSnapshotCtx = nullptr;
    bool (*loadPushStatusJson)(String& json, void* ctx) = nullptr;
    void* loadPushStatusJsonCtx = nullptr;
    bool (*applySlotUpdate)(const SlotUpdateRequest& request, void* ctx) = nullptr;
    void* applySlotUpdateCtx = nullptr;
    void (*setSlotName)(int slot, const String& name, void* ctx) = nullptr;
    void* setSlotNameCtx = nullptr;
    void (*setSlotColor)(int slot, uint16_t color, void* ctx) = nullptr;
    void* setSlotColorCtx = nullptr;
    uint8_t (*getSlotVolume)(int slot, void* ctx) = nullptr;
    void* getSlotVolumeCtx = nullptr;
    uint8_t (*getSlotMuteVolume)(int slot, void* ctx) = nullptr;
    void* getSlotMuteVolumeCtx = nullptr;
    void (*setSlotVolumes)(int slot, uint8_t volume, uint8_t muteVolume, void* ctx) = nullptr;
    void* setSlotVolumesCtx = nullptr;
    void (*setSlotDarkMode)(int slot, bool darkMode, void* ctx) = nullptr;
    void* setSlotDarkModeCtx = nullptr;
    void (*setSlotMuteToZero)(int slot, bool muteToZero, void* ctx) = nullptr;
    void* setSlotMuteToZeroCtx = nullptr;
    void (*setSlotAlertPersistSec)(int slot, uint8_t alertPersistSec, void* ctx) = nullptr;
    void* setSlotAlertPersistSecCtx = nullptr;
    void (*setSlotPriorityArrowOnly)(int slot, bool priorityArrowOnly, void* ctx) = nullptr;
    void* setSlotPriorityArrowOnlyCtx = nullptr;
    void (*setSlotProfileAndMode)(int slot, const String& profile, int mode, void* ctx) = nullptr;
    void* setSlotProfileAndModeCtx = nullptr;
    int (*getActiveSlot)(void* ctx) = nullptr;
    void* getActiveSlotCtx = nullptr;
    void (*drawProfileIndicator)(int slot, void* ctx) = nullptr;
    void* drawProfileIndicatorCtx = nullptr;
    bool (*applyActivation)(const ActivationRequest& request, void* ctx) = nullptr;
    void* applyActivationCtx = nullptr;
    void (*setActiveSlot)(int slot, void* ctx) = nullptr;
    void* setActiveSlotCtx = nullptr;
    void (*setAutoPushEnabled)(bool enabled, void* ctx) = nullptr;
    void* setAutoPushEnabledCtx = nullptr;
    ProfileAssignmentStatus (*validateProfileAssignment)(const String& canonicalProfile, void* ctx) = nullptr;
    void* validateProfileAssignmentCtx = nullptr;
    bool (*appendPushStatusJson)(JsonObject root, void* ctx) = nullptr;
    void* appendPushStatusJsonCtx = nullptr;
    bool (*loadSlotsSnapshotResult)(SlotsSnapshot& snapshot, void* ctx) = nullptr;
    void* loadSlotsSnapshotResultCtx = nullptr;
    V1SettingsOperationStore::StartResult (*startOperation)(const OperationStartRequest& request,
                                                            void* ctx) = nullptr;
    void* startOperationCtx = nullptr;
    bool (*loadOperation)(V1SettingsOperationStore::Snapshot& snapshot, void* ctx) = nullptr;
    void* loadOperationCtx = nullptr;
    OperationTargetStatus (*validateOperationTarget)(const String& canonicalAddress,
                                                     bool requireGen2, void* ctx) = nullptr;
    void* validateOperationTargetCtx = nullptr;
    void (*restartForOperation)(void* ctx) = nullptr;
    void* restartForOperationCtx = nullptr;
    bool (*profileHasVolumePolicy)(const String& canonicalProfile, void* ctx) = nullptr;
    void* profileHasVolumePolicyCtx = nullptr;
};

void handleApiSlots(WebServer& server, const Runtime& runtime);

void handleApiStatus(WebServer& server, const Runtime& runtime);
void handleApiStatusQuery(WebServer& server, const Runtime& runtime,
                          const uint8_t* query, size_t querySize);

void handleApiApplySlotBody(WebServer& server, const Runtime& runtime,
                            const uint8_t* body, size_t bodySize);
void handleApiApplyProfileBody(WebServer& server, const Runtime& runtime,
                               const uint8_t* body, size_t bodySize);
void handleApiFactoryResetBody(WebServer& server, const Runtime& runtime,
                               const uint8_t* body, size_t bodySize);

void handleApiSlotSave(WebServer& server, const Runtime& runtime, bool (*checkRateLimit)(void* ctx),
                       void* rateLimitCtx);
void handleApiSlotSaveBody(WebServer& server, const Runtime& runtime, const uint8_t* body, size_t bodySize,
                           bool (*checkRateLimit)(void* ctx), void* rateLimitCtx,
                           const char* multipartBoundary = nullptr, size_t multipartBoundarySize = 0);

void handleApiActivate(WebServer& server, const Runtime& runtime, bool (*checkRateLimit)(void* ctx),
                       void* rateLimitCtx);
void handleApiActivateBody(WebServer& server, const Runtime& runtime, const uint8_t* body, size_t bodySize,
                           bool (*checkRateLimit)(void* ctx), void* rateLimitCtx,
                           const char* multipartBoundary = nullptr, size_t multipartBoundarySize = 0);

} // namespace WifiAutoPushApiService
