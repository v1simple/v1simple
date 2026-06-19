#pragma once

#include <WebServer.h>

#include <cstdint>

#include "../../settings.h"

namespace WifiAudioApiService {

struct Runtime {
    const V1Settings& (*getSettings)(void* ctx)                        = nullptr;
    void              (*applySettingsUpdate)(const AudioSettingsUpdate&,
                                            void* ctx)                 = nullptr;
    void              (*setAudioVolume)(uint8_t volume, void* ctx)     = nullptr;
    bool              (*checkRateLimit)(void* ctx)                     = nullptr;
    void* ctx = nullptr;
};

void handleApiGet(WebServer& server, const Runtime& runtime);
void handleApiSave(WebServer& server, const Runtime& runtime);

}  // namespace WifiAudioApiService
