#pragma once

#include <WebServer.h>

#include <cstdint>

#include "../../settings.h"

namespace WifiDisplayColorsApiService {

struct Runtime {
    const V1Settings& (*getSettings)(void* ctx) = nullptr;
    void* getSettingsCtx = nullptr;
    void (*applySettingsUpdate)(const DisplaySettingsUpdate& update, void* ctx) = nullptr;
    void* applySettingsUpdateCtx = nullptr;
    void (*resetDisplaySettings)(void* ctx) = nullptr;
    void* resetDisplaySettingsCtx = nullptr;
    void (*setDisplayBrightness)(uint8_t brightness, void* ctx) = nullptr;
    void* setDisplayBrightnessCtx = nullptr;
    void (*forceDisplayRedraw)(void* ctx) = nullptr;
    void* forceDisplayRedrawCtx = nullptr;
    void (*requestColorPreviewHoldMs)(uint32_t durationMs, void* ctx) = nullptr;
    void* requestColorPreviewHoldMsCtx = nullptr;
    bool (*isColorPreviewRunning)(void* ctx) = nullptr;
    void* isColorPreviewRunningCtx = nullptr;
    void (*cancelColorPreview)(void* ctx) = nullptr;
    void* cancelColorPreviewCtx = nullptr;
};

void handleApiGet(WebServer& server, const Runtime& runtime);

void handleApiSave(WebServer& server,
                   const Runtime& runtime,
                   bool (*checkRateLimit)(void* ctx), void* rateLimitCtx);

void handleApiReset(WebServer& server,
                    const Runtime& runtime,
                    bool (*checkRateLimit)(void* ctx), void* rateLimitCtx);

void handleApiPreview(WebServer& server,
                      const Runtime& runtime,
                      bool (*checkRateLimit)(void* ctx), void* rateLimitCtx);

void handleApiClear(WebServer& server,
                    const Runtime& runtime,
                    bool (*checkRateLimit)(void* ctx), void* rateLimitCtx);

}  // namespace WifiDisplayColorsApiService
