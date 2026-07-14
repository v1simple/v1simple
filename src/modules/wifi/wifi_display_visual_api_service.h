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
};

void handleSteps(WebServer& server, const Runtime& runtime);
void handleLayout(WebServer& server, const Runtime& runtime);
void handlePin(WebServer& server, const Runtime& runtime, bool (*checkRateLimit)(void* ctx), void* rateLimitCtx);
void handleFramebuffer(WebServer& server, const Runtime& runtime);
void handleFlushShadow(WebServer& server, const Runtime& runtime);
void handleClear(WebServer& server, const Runtime& runtime);

} // namespace WifiDisplayVisualApiService
