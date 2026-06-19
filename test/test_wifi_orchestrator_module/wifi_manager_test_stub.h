#pragma once

#include "../mocks/Arduino.h"
#include "../mocks/FS.h"
#include <ArduinoJson.h>

// Keep this guard aligned with the production header so the real wifi_manager.h
// becomes a no-op when the orchestrator header is included in this test.
#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

namespace WifiAutoPushApiService {
struct PushNowRequest {
    int slot = 0;
    bool hasProfileOverride = false;
    String profileName;
    bool hasModeOverride = false;
    int mode = 0;
};

enum class PushNowQueueResult : uint8_t {
    QUEUED = 0,
    V1_NOT_CONNECTED,
    ALREADY_IN_PROGRESS,
    NO_PROFILE_CONFIGURED,
    PROFILE_LOAD_FAILED,
};
}  // namespace WifiAutoPushApiService

class WiFiManager {
public:
    bool setupModeActive = false;
    bool startSetupModeResult = true;
    int startSetupModeCalls = 0;
    bool lastStartAutoStarted = false;

    int statusCallbackCalls = 0;
    int alertCallbackCalls = 0;
    int filesystemCallbackCalls = 0;
    int pushStatusCallbackCalls = 0;
    int pushNowCallbackCalls = 0;
    int v1ConnectedCallbackCalls = 0;

    void reset() {
        setupModeActive = false;
        startSetupModeResult = true;
        startSetupModeCalls = 0;
        lastStartAutoStarted = false;
        statusCallbackCalls = 0;
        alertCallbackCalls = 0;
        filesystemCallbackCalls = 0;
        pushStatusCallbackCalls = 0;
        pushNowCallbackCalls = 0;
        v1ConnectedCallbackCalls = 0;
        statusCallback = nullptr;
        statusCallbackCtx = nullptr;
        statusCallback2 = nullptr;
        statusCallback2Ctx = nullptr;
        alertCallback = nullptr;
        alertCallbackCtx = nullptr;
        filesystemCallback = nullptr;
        filesystemCallbackCtx = nullptr;
        pushStatusCallback = nullptr;
        pushStatusCallbackCtx = nullptr;
        pushNowCallback = nullptr;
        pushNowCallbackCtx = nullptr;
        v1ConnectedCallback = nullptr;
        v1ConnectedCallbackCtx = nullptr;
    }

    bool isSetupModeActive() const { return setupModeActive; }

    bool startSetupMode(bool autoStarted = false) {
        ++startSetupModeCalls;
        lastStartAutoStarted = autoStarted;
        return startSetupModeResult;
    }

    void setStatusCallback(void (*fn)(ArduinoJson::JsonObject, void*), void* ctx) {
        ++statusCallbackCalls;
        statusCallback = fn;
        statusCallbackCtx = ctx;
    }

    void appendStatusCallback(void (*fn)(ArduinoJson::JsonObject, void*), void* ctx) {
        ++statusCallbackCalls;
        statusCallback2 = fn;
        statusCallback2Ctx = ctx;
    }

    void setAlertCallback(void (*fn)(ArduinoJson::JsonObject, void*), void* ctx) {
        ++alertCallbackCalls;
        alertCallback = fn;
        alertCallbackCtx = ctx;
    }

    void setFilesystemCallback(fs::FS* (*fn)(void*), void* ctx) {
        ++filesystemCallbackCalls;
        filesystemCallback = fn;
        filesystemCallbackCtx = ctx;
    }

    void setPushStatusCallback(String (*fn)(void*), void* ctx) {
        ++pushStatusCallbackCalls;
        pushStatusCallback = fn;
        pushStatusCallbackCtx = ctx;
    }

    void setPushNowCallback(
        WifiAutoPushApiService::PushNowQueueResult (*fn)(
            const WifiAutoPushApiService::PushNowRequest&, void*),
        void* ctx) {
        ++pushNowCallbackCalls;
        pushNowCallback = fn;
        pushNowCallbackCtx = ctx;
    }

    void setV1ConnectedCallback(bool (*fn)(void*), void* ctx) {
        ++v1ConnectedCallbackCalls;
        v1ConnectedCallback = fn;
        v1ConnectedCallbackCtx = ctx;
    }

    void setObdDependencies(void* /*obd*/, void* /*speed*/) {}

private:
    void (*statusCallback)(ArduinoJson::JsonObject, void*) = nullptr;
    void* statusCallbackCtx = nullptr;
    void (*statusCallback2)(ArduinoJson::JsonObject, void*) = nullptr;
    void* statusCallback2Ctx = nullptr;
    void (*alertCallback)(ArduinoJson::JsonObject, void*) = nullptr;
    void* alertCallbackCtx = nullptr;
    fs::FS* (*filesystemCallback)(void*) = nullptr;
    void* filesystemCallbackCtx = nullptr;
    String (*pushStatusCallback)(void*) = nullptr;
    void* pushStatusCallbackCtx = nullptr;
    WifiAutoPushApiService::PushNowQueueResult (*pushNowCallback)(
        const WifiAutoPushApiService::PushNowRequest&, void*) = nullptr;
    void* pushNowCallbackCtx = nullptr;
    bool (*v1ConnectedCallback)(void*) = nullptr;
    void* v1ConnectedCallbackCtx = nullptr;
};

#endif  // WIFI_MANAGER_H
