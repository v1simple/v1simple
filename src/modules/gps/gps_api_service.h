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

} // namespace GpsApiService
