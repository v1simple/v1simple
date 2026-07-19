#pragma once

#include <WebServer.h>

class GpsRuntimeModule;
class SettingsManager;

namespace GpsApiService {

struct Runtime {
    void (*markUiActivity)(void* ctx) = nullptr;
    void* ctx = nullptr;
    bool maintenanceBootActive = false;
};

// GET /api/gps/config — returns current GPS settings.
void handleApiConfigGet(WebServer& server, SettingsManager& settings, const Runtime& runtime);

// POST /api/gps/config — saves GPS settings (any subset of fields).
void handleApiConfigSave(WebServer& server, SettingsManager& settings, GpsRuntimeModule* gpsRuntime,
                         const Runtime& runtime);

inline void handleApiConfigSave(WebServer& server, SettingsManager& settings, GpsRuntimeModule& gpsRuntime,
                                const Runtime& runtime) {
    handleApiConfigSave(server, settings, &gpsRuntime, runtime);
}

// GET /api/gps/status — returns live GpsRuntimeStatus snapshot as JSON.
void handleApiStatus(WebServer& server, GpsRuntimeModule* gpsRuntime, const Runtime& runtime);

inline void handleApiStatus(WebServer& server, GpsRuntimeModule& gpsRuntime, const Runtime& runtime) {
    handleApiStatus(server, &gpsRuntime, runtime);
}

} // namespace GpsApiService
