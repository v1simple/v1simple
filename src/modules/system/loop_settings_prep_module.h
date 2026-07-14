#pragma once

#include <stdint.h>

struct LoopSettingsPrepValues {
    bool enableWifi = true;
};

struct LoopSettingsPrepContext {
    uint32_t nowMs = 0;
};

// Orchestrates tap-gesture processing and loop settings snapshot reads.
class LoopSettingsPrepModule {
  public:
    struct Providers {
        void (*runTapGesture)(void* ctx, uint32_t nowMs) = nullptr;
        void* tapGestureContext = nullptr;

        LoopSettingsPrepValues (*readSettingsValues)(void* ctx) = nullptr;
        void* settingsContext = nullptr;
    };

    void begin(const Providers& hooks);
    LoopSettingsPrepValues process(const LoopSettingsPrepContext& ctx);

  private:
    Providers providers{};
};
