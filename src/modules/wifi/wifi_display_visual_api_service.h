#pragma once

#include <WebServer.h>

#include "../../settings.h"
#include "../display/display_preview_module.h"
#include "display.h"

namespace WifiDisplayVisualApiService {

struct Runtime {
    DisplayPreviewModule* preview = nullptr;
    V1Display* display = nullptr;
    const V1Settings& (*getSettings)(void* ctx) = nullptr;
    void* getSettingsCtx = nullptr;
    const char* firmwareVersion = nullptr;
    const char* firmwareSha = nullptr;
    bool maintenanceBootActive = false;
    // Network address to restore onto the maintenance screen after a preview is
    // cleared. stationMode=true → maintenanceIp is the STA/DHCP address; false →
    // the setup AP's default address. Empty until WiFi is up.
    String maintenanceIp;
    bool maintenanceStationMode = false;
};

void handleSteps(WebServer& server, const Runtime& runtime);
void handleLayout(WebServer& server, const Runtime& runtime);
void handlePin(WebServer& server, const Runtime& runtime, bool (*checkRateLimit)(void* ctx), void* rateLimitCtx);
void handleFramebuffer(WebServer& server, const Runtime& runtime);
void handleFlushShadow(WebServer& server, const Runtime& runtime);
void handleClear(WebServer& server, const Runtime& runtime);

} // namespace WifiDisplayVisualApiService
