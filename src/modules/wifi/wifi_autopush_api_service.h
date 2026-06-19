#pragma once

#include <Arduino.h>
#include <WebServer.h>

#include <cstdint>

namespace WifiAutoPushApiService {

struct SlotConfig {
    String name;
    String profile;
    int mode = 0;
    uint16_t color = 0;
    uint8_t volume = 0;
    uint8_t muteVolume = 0;
    bool darkMode = false;
    bool muteToZero = false;
    uint8_t alertPersist = 0;
    bool priorityArrowOnly = false;
};

struct SlotsSnapshot {
    bool enabled = false;
    int activeSlot = 0;
    SlotConfig slots[3];
};

struct PushNowRequest {
    int slot = 0;
    bool hasProfileOverride = false;
    String profileName;
    bool hasModeOverride = false;
    int mode = 0;
};

struct SlotUpdateRequest {
    int slot = 0;
    bool hasName = false;
    String name;
    bool hasColor = false;
    uint16_t color = 0;
    bool hasVolume = false;
    uint8_t volume = 0;
    bool hasMuteVolume = false;
    uint8_t muteVolume = 0;
    bool hasDarkMode = false;
    bool darkMode = false;
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

enum class PushNowQueueResult : uint8_t {
    QUEUED = 0,
    V1_NOT_CONNECTED,
    ALREADY_IN_PROGRESS,
    NO_PROFILE_CONFIGURED,
    PROFILE_LOAD_FAILED,
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
    PushNowQueueResult (*queuePushNow)(const PushNowRequest& request, void* ctx) = nullptr;
    void* queuePushNowCtx = nullptr;
};

void handleApiSlots(WebServer& server, const Runtime& runtime);

void handleApiStatus(WebServer& server, const Runtime& runtime);

void handleApiSlotSave(WebServer& server,
                       const Runtime& runtime,
                       bool (*checkRateLimit)(void* ctx), void* rateLimitCtx);

void handleApiActivate(WebServer& server,
                       const Runtime& runtime,
                       bool (*checkRateLimit)(void* ctx), void* rateLimitCtx);

void handleApiPushNow(WebServer& server,
                      const Runtime& runtime,
                      bool (*checkRateLimit)(void* ctx), void* rateLimitCtx);

}  // namespace WifiAutoPushApiService
