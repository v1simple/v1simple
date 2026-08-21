#pragma once

#include <cstdint>

#include "../../settings.h"

struct WifiAudioSettingsRuntime {
    const V1Settings& (*getSettings)(void* ctx) = nullptr;
    void (*applySettingsUpdate)(const AudioSettingsUpdate&, void* ctx) = nullptr;
    void (*setAudioVolume)(uint8_t volume, void* ctx) = nullptr;
    bool (*checkRateLimit)(void* ctx) = nullptr;
    void* ctx = nullptr;
};
