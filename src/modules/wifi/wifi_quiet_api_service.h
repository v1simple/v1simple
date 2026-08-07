#pragma once

#include <WebServer.h>

#include "../../settings.h"

namespace WifiQuietApiService {

struct Runtime {
    const V1Settings& (*getSettings)(void* ctx) = nullptr;
    void (*applySettingsUpdate)(const QuietSettingsUpdate&, void* ctx) = nullptr;
    bool (*checkRateLimit)(void* ctx) = nullptr;
    void* ctx = nullptr;
};

void handleApiGet(WebServer& server, const Runtime& runtime);
void handleApiSave(WebServer& server, const Runtime& runtime);

} // namespace WifiQuietApiService
