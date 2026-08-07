#pragma once

#include <Arduino.h>
#include <WebServer.h>

#include <cstdint>

#include "../../settings.h"

namespace WifiSettingsApiService {

struct Runtime {
    const V1Settings& (*getSettings)(void* ctx) = nullptr;
    void (*applySettingsUpdate)(const DeviceSettingsUpdate&, void* ctx) = nullptr;
    bool (*checkRateLimit)(void* ctx) = nullptr;
    SettingsManager::NvsDiagnostic (*getNvsDiagnostic)(void* ctx) = nullptr;
    void* ctx = nullptr;
};

void handleApiDeviceSettingsGet(WebServer& server, const Runtime& runtime);

void handleApiDeviceSettingsSave(WebServer& server, const Runtime& runtime);

} // namespace WifiSettingsApiService
