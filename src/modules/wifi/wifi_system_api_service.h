#pragma once

#include <WebServer.h>

namespace WifiSystemApiService {

struct RebootRuntime {
    bool maintenanceBootActive = false;
    void (*persistSettings)(void* ctx) = nullptr;
    void (*markCleanShutdown)(void* ctx) = nullptr;
    void (*delayBeforeRestart)(uint32_t delayMs, void* ctx) = nullptr;
    void (*restart)(void* ctx) = nullptr;
    void (*markUiActivity)(void* ctx) = nullptr;
    void* ctx = nullptr;
};

// POST /api/system/reboot-normal. Route composition must additionally enforce
// the maintenance write header before calling this handler.
void handleApiRebootNormal(WebServer& server, const RebootRuntime& runtime);

} // namespace WifiSystemApiService
