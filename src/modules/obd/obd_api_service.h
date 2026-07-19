#pragma once

#include <WebServer.h>

class ObdRuntimeModule;
class SettingsManager;

namespace ObdApiService {

struct Runtime {
    void (*markUiActivity)(void* ctx) = nullptr;
    bool (*checkRateLimit)(void* ctx) = nullptr;
    void (*syncAfterConfigChange)(void* ctx) = nullptr;
    void* ctx = nullptr;
    bool maintenanceBootActive = false;
};

void handleApiConfigGet(WebServer& server, SettingsManager& settings, const Runtime& runtime);

void handleApiStatus(WebServer& server, ObdRuntimeModule* obdRuntime, const Runtime& runtime);

inline void handleApiStatus(WebServer& server, ObdRuntimeModule& obdRuntime, const Runtime& runtime) {
    handleApiStatus(server, &obdRuntime, runtime);
}

void handleApiDevicesList(WebServer& server, ObdRuntimeModule* obdRuntime, SettingsManager& settings,
                          const Runtime& runtime);

inline void handleApiDevicesList(WebServer& server, ObdRuntimeModule& obdRuntime, SettingsManager& settings,
                                 const Runtime& runtime) {
    handleApiDevicesList(server, &obdRuntime, settings, runtime);
}

void handleApiDeviceNameSave(WebServer& server, SettingsManager& settings, const Runtime& runtime);

void handleApiScan(WebServer& server, ObdRuntimeModule* obdRuntime, const Runtime& runtime);

inline void handleApiScan(WebServer& server, ObdRuntimeModule& obdRuntime, const Runtime& runtime) {
    handleApiScan(server, &obdRuntime, runtime);
}

void handleApiForget(WebServer& server, ObdRuntimeModule* obdRuntime, SettingsManager& settings,
                     const Runtime& runtime);

inline void handleApiForget(WebServer& server, ObdRuntimeModule& obdRuntime, SettingsManager& settings,
                            const Runtime& runtime) {
    handleApiForget(server, &obdRuntime, settings, runtime);
}

void handleApiConfig(WebServer& server, ObdRuntimeModule* obdRuntime, SettingsManager& settings,
                     const Runtime& runtime);

inline void handleApiConfig(WebServer& server, ObdRuntimeModule& obdRuntime, SettingsManager& settings,
                            const Runtime& runtime) {
    handleApiConfig(server, &obdRuntime, settings, runtime);
}

} // namespace ObdApiService
